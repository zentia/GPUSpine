// =====================================================================
// GPU-Spine 性能基准（CPU 侧，单线程）：我们的每帧路径 vs spine-cpp 3.8 的 CPU 蒙皮路径
// ---------------------------------------------------------------------
// 用法：SpineGpuBenchmark <资产目录> [--frames N] [--limit N] [--csv out.csv] [--top N] [--verbose] [--split]
//                         [--instances N]
//   目录里找 `*.skel.bytes` / `*.json` + 同名 `.atlas.txt` / `.atlas`（与 SpineGpuRefParityTest 同口径）。
//
// 每个资产两边吃**同一份**脚本：dt = 1/60，default mix 0.2，轨道 0 循环播第 0 个动画，每 60 帧
// setAnimation 到下一个动画（循环取）——覆盖"正常播放 + 混合"这两种游戏里最常见的状态。
//
// 我们（= SpineGpuRenderer::Impl 每帧对一个世界实例做的事，逐个函数调同一份代码）：
//   anim  : SpineGpuAnimationState::Update + Apply
//   pose  : SpineGpuUpdateWorld（骨骼世界变换 + 槽位颜色 / 附件 / 绘制顺序 + 生效的 mesh deform）
//   geo   : SpineGpuClipFrame（裁剪作用域内的项在 CPU 上蒙皮 + 裁）+ SpineGpuAppendBatchInstance（拼包）
//   上传  : 合包各数组的字节数（bones / draws / insts / deform / cpu_verts / cpu_index）——每帧要传给 GPU 的量
//
// spine-cpp（= spine-sfml / spine-cocos2dx / spine-unity 的 CPU 网格路径）：
//   anim  : AnimationState::update + apply
//   pose  : Skeleton::updateWorldTransform
//   geo   : 按 drawOrder 逐槽位 computeWorldVertices（region 4 顶点 / mesh 全部顶点）+ SkeletonClipping，
//           写成交错顶点（pos float2 + uv float2 + rgba8 [+ dark rgba8]）+ u16 索引 —— 这是**最省**的写法
//           （spine-unity 实际是 pos float3 + color + uv，更大），偏向参考那边
//   上传  : 顶点 + 索引字节数
//
// 输出的是**每实例**的成本（单线程）。`--instances N`：每个资产同时跑 N 个实例（起始时间错开），
// 两边都按"一帧把 N 个实例拼进同一个合包 / 同一个动态顶点缓冲"的方式做 —— 与真实渲染器一致。
//
// 内存（2026-10-29 加）：两边的堆**常驻字节**都按"共享（每资产一份）"与"每实例"拆开：
//   我们    共享 = SpineGpuAsset（CPU 侧容器）+ SpineGpuRuntimeData + 合包资产信息；另有静态显存（VB/影响流/UV/IB）
//           每实例 = 骨架状态 + AnimationState + 姿势 + 裁剪器 + 合包里它那一份（稳态，跑完热身之后量）
//   spine-cpp 共享 = SkeletonData；每实例 = Skeleton + AnimationState + 裁剪器 + 它那份 CPU 顶点 / 索引
// ⚠️ spine-cpp 的内存**不走 operator new**：`SpineObject::operator new` 与 `SpineExtension::alloc`
//    都落到 `DefaultSpineExtension::_alloc` → `::malloc`。所以下面装了一个计数的 SpineExtension，
//    否则它的分配次数 / 常驻字节全是 0（2026-10-23 那版"spine-cpp 稳态分配合计 11 次"就是只数到了
//    基准自己的 std::vector，spine-cpp 内部一次都没数到）。
// 编出来必须是 Release / RelWithDebInfo（Debug 的数字没有意义，main() 里会警告）。
// ⚠️ 分配计数同理：MSVC 的 Debug 迭代器调试给每个容器构造发一个 16 字节的
//    `_Container_proxy`（稳态约 8 次/帧，apply 7 + pose 1），那不是数据缓冲
//    ⇒ Debug 下"每帧分配次数"恒为非 0，只有 Release 的数字能读。
// stdout 不缓冲：经 MSYS 管道重定向时，缓冲的输出在进程退出时会丢（实测）。
// ⚠️ 绝对值受机器负载影响很大（同一份二进制连跑三次能漂 50%），**只能看比值**；
//    最新数字与结论记在 `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` §17（改口径要同步改那一节）。
// =====================================================================

#include "SpineGpuAnimationState.h"
#include "SpineGpuBatch.h"
#include "SpineGpuClipper.h"
#include "SpineGpuImporter.h"
#include "SpineGpuJsonFixture.h"
#include "SpineGpuPoseSolver.h"
#include "SpineGpuSkeletonState.h"

#include "SpineGpuRefRuntime.h"

#include <spine/Animation.h>
#include <spine/Extension.h>
#include <spine/AnimationState.h>
#include <spine/AnimationStateData.h>
#include <spine/Bone.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonClipping.h>
#include <spine/SkeletonData.h>
#include <spine/SkeletonJson.h>
#include <spine/Slot.h>
#include <spine/SlotData.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <malloc.h>
#include <new>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// =====================================================================
// 每帧堆分配计数
// ---------------------------------------------------------------------
// 热路径里每帧 malloc/free 是这套代码犯过的最贵的一个错（deform 展开：每条时间轴
// 每帧 2~4 次 `std::vector` 分配，占 anim 段的 ~29%），而它**不报错、不画错**，
// 只会让 anim 段悄悄贵一倍 ⇒ 把它变成能看见的数字（两边各数一遍）。
// 做法：替换全局 `operator new/delete`，只在开关打开时累加。
// =====================================================================
namespace
{
bool g_alloc_counting = false;
long long g_alloc_calls = 0;
long long g_alloc_bytes = 0;
// 常驻字节（始终在记）：每块前面藏 16 B 的头存请求大小，free 时减回去。
// 两边（operator new 与 spine 的 SpineExtension）走同一套 ⇒ 常驻字节可以直接比。
long long g_live_bytes = 0;
constexpr std::size_t kHdr = 16;   // 保持 16 B 对齐

void* TrackedAlloc(std::size_t n)
{
    unsigned char* raw = static_cast<unsigned char*>(std::malloc(n + kHdr));
    if (raw == nullptr)
    {
        return nullptr;
    }
    std::memcpy(raw, &n, sizeof(n));
    g_live_bytes += static_cast<long long>(n);
    if (g_alloc_counting)
    {
        ++g_alloc_calls;
        g_alloc_bytes += static_cast<long long>(n);
    }
    return raw + kHdr;
}
void TrackedFree(void* p)
{
    if (p == nullptr)
    {
        return;
    }
    unsigned char* raw = static_cast<unsigned char*>(p) - kHdr;
    std::size_t n = 0;
    std::memcpy(&n, raw, sizeof(n));
    g_live_bytes -= static_cast<long long>(n);
    std::free(raw);
}
void* TrackedRealloc(void* p, std::size_t n)
{
    if (p == nullptr)
    {
        return TrackedAlloc(n);
    }
    unsigned char* raw = static_cast<unsigned char*>(p) - kHdr;
    std::size_t old = 0;
    std::memcpy(&old, raw, sizeof(old));
    unsigned char* nr = static_cast<unsigned char*>(std::realloc(raw, n + kHdr));
    if (nr == nullptr)
    {
        return nullptr;
    }
    std::memcpy(nr, &n, sizeof(n));
    g_live_bytes += static_cast<long long>(n) - static_cast<long long>(old);
    if (g_alloc_counting)
    {
        ++g_alloc_calls;
        g_alloc_bytes += static_cast<long long>(n);
    }
    return nr + kHdr;
}
}   // namespace

void* operator new(std::size_t n)
{
    void* p = TrackedAlloc(n != 0u ? n : 1u);
    if (p == nullptr)
    {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { TrackedFree(p); }
void operator delete[](void* p) noexcept { TrackedFree(p); }
void operator delete(void* p, std::size_t) noexcept { TrackedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { TrackedFree(p); }
// 对齐版 new 没有替换（两边都没用到超对齐类型）。对齐版 delete 原样交还 CRT 的对齐释放，
// 绝不能落到 TrackedFree（那块内存不是我们分的，没有头）。
void operator delete(void* p, std::align_val_t) noexcept { ::_aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { ::_aligned_free(p); }

namespace
{
// spine-cpp 的分配全部走这里（SpineObject::operator new / SpineExtension::alloc）。
class CountingSpineExtension : public spine::DefaultSpineExtension
{
protected:
    void* _alloc(size_t size, const char*, int) override { return size == 0 ? nullptr : TrackedAlloc(size); }
    void* _calloc(size_t size, const char*, int) override
    {
        if (size == 0)
        {
            return nullptr;
        }
        void* p = TrackedAlloc(size);
        if (p != nullptr)
        {
            std::memset(p, 0, size);
        }
        return p;
    }
    void* _realloc(void* ptr, size_t size, const char*, int) override
    {
        if (size == 0)
        {
            TrackedFree(ptr);
            return nullptr;
        }
        return TrackedRealloc(ptr, size);
    }
    void _free(void* mem, const char*, int) override { TrackedFree(mem); }
};
CountingSpineExtension g_counting_extension;
}   // namespace

namespace
{
void AllocCountStart()
{
    g_alloc_calls = 0;
    g_alloc_bytes = 0;
    g_alloc_counting = true;
}
void AllocCountStop(long long& calls, long long& bytes)
{
    g_alloc_counting = false;
    calls = g_alloc_calls;
    bytes = g_alloc_bytes;
}
}   // namespace

namespace
{
using Clock = std::chrono::steady_clock;
inline double UsSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}
inline double UsBetween(Clock::time_point t0, Clock::time_point t1)
{
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

bool ReadAll(const fs::path& p, std::vector<char>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// 一个资产在一边上跑完脚本后的结果（每帧平均）
struct SideResult
{
    double anim_us = 0.0;
    double pose_us = 0.0;
    double geo_us = 0.0;     // 我们：clip + batch；参考：顶点生成 + 裁剪
    double total_us = 0.0;
    double upload_bytes = 0.0;
    double vertices = 0.0;   // 参考：每帧生成的顶点数；我们：CPU 裁剪顶点数
    double allocs_per_frame = 0.0;   // 稳态每帧的堆分配次数（>0 = 热路径里还有多余的活）
    double alloc_bytes_per_frame = 0.0;
    // `--split`：anim 段细拆成 Update（推进轨道 / 排队事件）与 Apply（逐时间轴写状态）。
    // 两边同一个切法（参考侧也是 AnimationState::update + apply）⇒ 可以直接对拍。
    double anim_update_us = 0.0;
    double anim_apply_us = 0.0;
    double anim_timelines = 0.0;     // 每帧实际 apply 的时间轴条数（脚本里会切动画 ⇒ 取平均）
    // 内存（字节，堆常驻）：shared = 每资产一份；per_instance = (N 个实例稳态常驻) / N
    double shared_bytes = 0.0;
    double per_instance_bytes = 0.0;
};

struct AssetResult
{
    std::string name;
    uint32_t bones = 0;
    uint32_t slots = 0;
    uint32_t anims = 0;
    uint32_t iks = 0;
    uint32_t transforms = 0;
    uint32_t paths = 0;
    uint64_t static_gpu_bytes = 0;   // 我们一次性上传、常驻显存的静态数据（VB + 影响流 + UV + IB + 绘制项）
    double ours_container_bytes = 0.0;   // 我们：SpineGpuAsset（CPU 侧容器）的堆常驻
    double ref_skeleton_data_bytes = 0.0;   // spine-cpp：SkeletonData 的堆常驻
    SideResult ours;
    SideResult ref;
};

constexpr float kDt = 1.0f / 60.0f;
constexpr uint32_t kSwitchEvery = 60;

// ---- 我们 ----
// `alloc_probe` > 0：在**最后 `alloc_probe` 帧**打开堆分配计数（前面的帧留给"首次分配扩容"，
// 所以数出来的就是稳态每帧的分配次数）。计时不受影响地跑在同一趟里 —— 计数只是每分配多一个
// 分支，且只落在末尾这几帧。
bool RunOurs(const SpineGpuAsset& asset, uint32_t frames, SideResult& out, std::string& error, uint32_t alloc_probe = 0,
             uint32_t instances = 1)
{
    // 共享（每资产一份）：运行时数据 + 合包资产信息。容器本身（SpineGpuAsset）在 main 里量。
    const long long live0 = g_live_bytes;
    SpineGpuRuntimeData data;
    SpineImportResult r = SpineGpuBuildRuntimeData(asset, data);
    if (!r.ok)
    {
        error = r.error;
        return false;
    }
    SpineGpuBatchAssetInfo info;
    SpineGpuBuildBatchAssetInfo(asset, info);
    SpineGpuAnimationStateData mix;
    mix.default_mix = 0.2f;
    const long long live_shared = g_live_bytes;

    // 每实例：骨架状态 + AnimationState + 姿势 + 裁剪器 / 裁剪结果；合包所有实例共用一个（拼包）
    struct Inst
    {
        SpineGpuSkeletonState state;
        std::unique_ptr<SpineGpuAnimationState> anim;
        SpineGpuPoseResult pose;
        SpineGpuSkeletonClipping clipper;
        SpineGpuClipFrameResult clip;
        uint32_t cur_anim = 0;
    };
    const uint32_t anim_count = static_cast<uint32_t>(asset.anims.size());
    std::vector<std::unique_ptr<Inst>> insts;
    insts.reserve(instances);
    for (uint32_t i = 0; i < instances; ++i)
    {
        auto in = std::make_unique<Inst>();
        r = SpineGpuInitSkeletonState(asset, data, in->state);
        if (!r.ok)
        {
            error = r.error;
            return false;
        }
        in->anim = std::make_unique<SpineGpuAnimationState>(asset, data, &mix);
        // 错开起点：实例 i 从第 (i % 动画数) 个动画、往前推 i·0.37 s 开始 ⇒ N 个实例不是同一帧的拷贝
        in->cur_anim = anim_count > 0 ? i % anim_count : 0u;
        in->anim->SetAnimation(0, in->cur_anim, true);
        if (i > 0)
        {
            in->anim->Update(0.37f * static_cast<float>(i));
        }
        insts.push_back(std::move(in));
    }
    SpineGpuBatch batch;
    SpineGpuGpuInst gpu_inst {};
    for (int i = 0; i < 16; i += 5)
    {
        gpu_inst.clip_from_world[i] = 1.0f;   // 单位阵：拼包只抄进去，值不影响成本
    }
    gpu_inst.root_abcd[0] = gpu_inst.root_abcd[3] = 1.0f;
    const bool tint_black = (asset.header.flags & SpineGpuFlags::kUsesTintBlack) != 0u;
    double anim_us = 0, pose_us = 0, geo_us = 0, bytes = 0, verts = 0;
    double anim_update_us = 0, anim_apply_us = 0, tl_total = 0;
    const uint32_t probe_begin = (alloc_probe != 0u && frames > alloc_probe) ? (frames - alloc_probe) : 0u;
    for (uint32_t f = 0; f < frames; ++f)
    {
        if (alloc_probe != 0u && f == probe_begin)
        {
            AllocCountStart();
        }
        batch.Clear();
        for (auto& in : insts)
        {
            if (f > 0 && f % kSwitchEvery == 0 && anim_count > 1)
            {
                in->cur_anim = (in->cur_anim + 1u) % anim_count;
                in->anim->SetAnimation(0, in->cur_anim, true);
            }
            auto t0 = Clock::now();
            in->anim->Update(kDt);
            auto t1 = Clock::now();
            r = in->anim->Apply(in->state);
            auto t2 = Clock::now();
            anim_us += UsBetween(t0, t2);
            anim_update_us += UsBetween(t0, t1);
            anim_apply_us += UsBetween(t1, t2);
            if (!r.ok)
            {
                error = "apply: " + r.error;
                return false;
            }
            t0 = Clock::now();
            r = SpineGpuUpdateWorld(asset, data, in->state, in->pose);
            pose_us += UsSince(t0);
            if (!r.ok)
            {
                error = "pose: " + r.error;
                return false;
            }
            t0 = Clock::now();
            r = SpineGpuClipFrame(asset, in->state, in->pose, in->clipper, in->clip);
            if (r.ok)
            {
                SpineGpuBatchRange range;
                r = SpineGpuAppendBatchInstance(asset, info, in->pose, &in->clip, gpu_inst, tint_black, batch, range);
            }
            geo_us += UsSince(t0);
            if (!r.ok)
            {
                error = "clip/batch: " + r.error;
                return false;
            }
            tl_total += static_cast<double>(anim_count > 0 ? asset.anims[in->cur_anim].timeline_count : 0u);
        }
        bytes += static_cast<double>(batch.bones.size() * sizeof(SpineGpuBoneSample) +
                                     batch.draws.size() * sizeof(SpineGpuGpuDraw) +
                                     batch.insts.size() * sizeof(SpineGpuGpuInst) + batch.deform.size() * sizeof(float) +
                                     batch.cpu_verts.size() * sizeof(SpineGpuCpuVert) +
                                     batch.cpu_index.size() * sizeof(uint16_t));
        verts += static_cast<double>(batch.cpu_verts.size());
        if (alloc_probe != 0u && f + 1u == frames)
        {
            long long calls = 0, nbytes = 0;
            AllocCountStop(calls, nbytes);
            out.allocs_per_frame = static_cast<double>(calls) / static_cast<double>(alloc_probe) / instances;
            out.alloc_bytes_per_frame = static_cast<double>(nbytes) / static_cast<double>(alloc_probe) / instances;
        }
    }
    // 稳态常驻（跑完全部帧之后，vector 都已扩到稳态容量）
    out.shared_bytes = static_cast<double>(live_shared - live0);
    out.per_instance_bytes = static_cast<double>(g_live_bytes - live_shared) / instances;
    const double n = static_cast<double>(frames) * instances;   // 每实例每帧
    out.anim_us = anim_us / n;
    out.pose_us = pose_us / n;
    out.geo_us = geo_us / n;
    out.total_us = out.anim_us + out.pose_us + out.geo_us;
    out.upload_bytes = bytes / n;
    out.vertices = verts / n;
    out.anim_update_us = anim_update_us / n;
    out.anim_apply_us = anim_apply_us / n;
    out.anim_timelines = tl_total / n;
    return true;
}

// ---- spine-cpp 的 CPU 网格路径 ----
struct RefVertex
{
    float x, y, u, v;
    uint32_t rgba;
};
inline uint32_t PackColor(float r, float g, float b, float a)
{
    const auto c = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

bool RunRef(spine::SkeletonData* data, uint32_t frames, SideResult& out, std::string& error, uint32_t alloc_probe = 0,
            uint32_t instances = 1)
{
    spine::Vector<spine::Animation*>& anims = data->getAnimations();
    if (anims.size() == 0)
    {
        error = "no animations";
        return false;
    }
    const long long live0 = g_live_bytes;
    // AnimationStateData 是 spine-unity 里 SkeletonDataAsset 持有的那份（每资产一份）⇒ 记进共享
    spine::AnimationStateData state_data(data);
    state_data.setDefaultMix(0.2f);
    const long long live_shared = g_live_bytes;
    struct Inst
    {
        std::unique_ptr<spine::Skeleton> skeleton;
        std::unique_ptr<spine::AnimationState> state;
        spine::SkeletonClipping clipper;
        uint32_t cur_anim = 0;
    };
    const uint32_t anim_count = static_cast<uint32_t>(anims.size());
    std::vector<std::unique_ptr<Inst>> insts;
    insts.reserve(instances);
    for (uint32_t i = 0; i < instances; ++i)
    {
        auto in = std::make_unique<Inst>();
        in->skeleton = std::make_unique<spine::Skeleton>(data);
        in->state = std::make_unique<spine::AnimationState>(&state_data);
        in->cur_anim = i % anim_count;
        in->state->setAnimation(0, anims[in->cur_anim], true);
        if (i > 0)
        {
            in->state->update(0.37f * static_cast<float>(i));
        }
        insts.push_back(std::move(in));
    }
    spine::Vector<float> world;
    // 动态顶点缓冲：N 个实例拼进同一个（spine-unity 是每实例一个 Mesh，那只会更大）
    std::vector<RefVertex> vb;
    std::vector<uint16_t> ib;   // ⚠️ 多实例时 u16 会溢出 —— 只算成本，不拿来画，按块计数即可
    std::vector<uint32_t> dark;   // 有暗色的槽位多一个 rgba8（spine 的 two-color tint）
    static unsigned short kQuad[6] = {0, 1, 2, 2, 3, 0};
    double anim_us = 0, pose_us = 0, geo_us = 0, bytes = 0, verts = 0;
    double anim_update_us = 0, anim_apply_us = 0, tl_total = 0;
    const uint32_t probe_begin = (alloc_probe != 0u && frames > alloc_probe) ? (frames - alloc_probe) : 0u;
    for (uint32_t f = 0; f < frames; ++f)
    {
        if (alloc_probe != 0u && f == probe_begin)
        {
            AllocCountStart();
        }
        vb.clear();
        ib.clear();
        dark.clear();
        for (auto& in : insts)
        {
            spine::Skeleton& skeleton = *in->skeleton;
            spine::AnimationState& state = *in->state;
            spine::SkeletonClipping& clipper = in->clipper;
            if (f > 0 && f % kSwitchEvery == 0 && anim_count > 1)
            {
                in->cur_anim = (in->cur_anim + 1u) % anim_count;
                state.setAnimation(0, anims[in->cur_anim], true);
            }
            auto t0 = Clock::now();
            state.update(kDt);
            auto t1 = Clock::now();
            state.apply(skeleton);
            auto t2 = Clock::now();
            anim_us += UsBetween(t0, t2);
            anim_update_us += UsBetween(t0, t1);
            anim_apply_us += UsBetween(t1, t2);
            t0 = Clock::now();
            skeleton.updateWorldTransform();
            pose_us += UsSince(t0);

            t0 = Clock::now();
            const spine::Color& sc = skeleton.getColor();
            spine::Vector<spine::Slot*>& order = skeleton.getDrawOrder();
            for (size_t i = 0; i < order.size(); ++i)
            {
                spine::Slot& slot = *order[i];
                spine::Attachment* att = slot.getAttachment();
                if (att == nullptr)
                {
                    clipper.clipEnd(slot);
                    continue;
                }
                if (slot.getColor().a == 0.0f)
                {
                    clipper.clipEnd(slot);
                    continue;
                }
                float* positions = nullptr;
                float* uvs = nullptr;
                unsigned short* tris = nullptr;
                size_t vcount = 0;
                size_t tcount = 0;
                const spine::Color* ac = nullptr;
                if (att->getRTTI().isExactly(spine::RegionAttachment::rtti))
                {
                    auto* ra = static_cast<spine::RegionAttachment*>(att);
                    world.setSize(8, 0.0f);
                    ra->computeWorldVertices(slot.getBone(), world, 0, 2);
                    positions = world.buffer();
                    uvs = ra->getUVs().buffer();
                    tris = kQuad;
                    vcount = 4;
                    tcount = 6;
                    ac = &ra->getColor();
                }
                else if (att->getRTTI().isExactly(spine::MeshAttachment::rtti))
                {
                    auto* ma = static_cast<spine::MeshAttachment*>(att);
                    const size_t len = ma->getWorldVerticesLength();
                    world.setSize(len, 0.0f);
                    ma->computeWorldVertices(slot, 0, len, world, 0, 2);
                    positions = world.buffer();
                    uvs = ma->getUVs().buffer();
                    tris = ma->getTriangles().buffer();
                    vcount = len / 2;
                    tcount = ma->getTriangles().size();
                    ac = &ma->getColor();
                }
                else if (att->getRTTI().isExactly(spine::ClippingAttachment::rtti))
                {
                    clipper.clipStart(slot, static_cast<spine::ClippingAttachment*>(att));
                    continue;
                }
                else
                {
                    clipper.clipEnd(slot);
                    continue;
                }
                if (clipper.isClipping())
                {
                    clipper.clipTriangles(positions, tris, tcount, uvs, 2);
                    positions = clipper.getClippedVertices().buffer();
                    uvs = clipper.getClippedUVs().buffer();
                    tris = clipper.getClippedTriangles().buffer();
                    vcount = clipper.getClippedVertices().size() / 2;
                    tcount = clipper.getClippedTriangles().size();
                }
                const spine::Color& lc = slot.getColor();
                const uint32_t rgba = PackColor(sc.r * lc.r * ac->r, sc.g * lc.g * ac->g, sc.b * lc.b * ac->b,
                                                sc.a * lc.a * ac->a);
                const uint16_t base = static_cast<uint16_t>(vb.size());
                for (size_t v = 0; v < vcount; ++v)
                {
                    vb.push_back(RefVertex {positions[v * 2], positions[v * 2 + 1], uvs[v * 2], uvs[v * 2 + 1], rgba});
                }
                if (slot.hasDarkColor())
                {
                    const spine::Color& dc = slot.getDarkColor();
                    dark.insert(dark.end(), vcount, PackColor(dc.r, dc.g, dc.b, 1.0f));
                }
                for (size_t t = 0; t < tcount; ++t)
                {
                    ib.push_back(static_cast<uint16_t>(base + tris[t]));
                }
                clipper.clipEnd(slot);
            }
            clipper.clipEnd();
            geo_us += UsSince(t0);
            tl_total += static_cast<double>(anims[in->cur_anim]->getTimelines().size());
        }
        bytes += static_cast<double>(vb.size() * sizeof(RefVertex) + dark.size() * sizeof(uint32_t) +
                                     ib.size() * sizeof(uint16_t));
        verts += static_cast<double>(vb.size());
        if (alloc_probe != 0u && f + 1u == frames)
        {
            long long calls = 0, nbytes = 0;
            AllocCountStop(calls, nbytes);
            out.allocs_per_frame = static_cast<double>(calls) / static_cast<double>(alloc_probe) / instances;
            out.alloc_bytes_per_frame = static_cast<double>(nbytes) / static_cast<double>(alloc_probe) / instances;
        }
    }
    out.shared_bytes = static_cast<double>(live_shared - live0);
    out.per_instance_bytes = static_cast<double>(g_live_bytes - live_shared) / instances;
    const double n = static_cast<double>(frames) * instances;
    out.anim_us = anim_us / n;
    out.pose_us = pose_us / n;
    out.geo_us = geo_us / n;
    out.total_us = out.anim_us + out.pose_us + out.geo_us;
    out.upload_bytes = bytes / n;
    out.vertices = verts / n;
    out.anim_update_us = anim_update_us / n;
    out.anim_apply_us = anim_apply_us / n;
    out.anim_timelines = tl_total / n;
    return true;
}

double Percentile(std::vector<double> v, double p)
{
    if (v.empty())
    {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    return v[i];
}

}   // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 不缓冲：见文件头
    spine::SpineExtension::setInstance(&g_counting_extension);   // spine-cpp 的分配也要数到（见文件头）
#if defined(_DEBUG)
    std::printf("⚠️ Debug 构建：下面的数字没有意义（STL 迭代器检查 + 无优化）。用 --config Release 编。\n");
#endif
    if (argc < 2)
    {
        std::printf("usage: SpineGpuBenchmark <asset dir> [--frames N] [--limit N] [--csv out.csv] [--top N] [--verbose] [--split] [--instances N]\n");
        return 2;
    }
    const fs::path dir = argv[1];
    uint32_t frames = 360;
    size_t limit = 0;
    size_t top = 10;
    std::string csv_path;
    bool verbose = false;
    bool split = false;
    uint32_t instances = 1;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
        {
            frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        }
        else if (a == "--limit" && i + 1 < argc)
        {
            limit = static_cast<size_t>(std::atoi(argv[++i]));
        }
        else if (a == "--csv" && i + 1 < argc)
        {
            csv_path = argv[++i];
        }
        else if (a == "--verbose")
        {
            verbose = true;
        }
        else if (a == "--split")
        {
            split = true;
        }
        else if (a == "--instances" && i + 1 < argc)
        {
            instances = std::max(1u, static_cast<uint32_t>(std::atoi(argv[++i])));
        }
        else if (a == "--top" && i + 1 < argc)
        {
            top = static_cast<size_t>(std::atoi(argv[++i]));
        }
    }

    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    {
        if (ec || !it->is_regular_file(ec))
        {
            continue;
        }
        const std::string n = it->path().filename().string();
        const bool is_skel = n.size() > 11 && n.compare(n.size() - 11, 11, ".skel.bytes") == 0;
        const bool is_json = n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0;
        if (is_skel || is_json)
        {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<AssetResult> results;
    size_t skipped = 0;
    std::string first_skip;
    const auto t_all = Clock::now();
    for (const fs::path& skel_path : files)
    {
        if (limit != 0 && results.size() >= limit)
        {
            break;
        }
        const std::string fname = skel_path.filename().string();
        if (verbose)
        {
            std::fprintf(stderr, "[bench] %s\n", skel_path.string().c_str());
            std::fflush(stderr);
        }
        const bool is_skel = fname.size() > 11 && fname.compare(fname.size() - 11, 11, ".skel.bytes") == 0;
        const std::string stem = fname.substr(0, fname.size() - (is_skel ? 11 : 5));
        fs::path atlas_path = skel_path.parent_path() / (stem + ".atlas.txt");
        if (!fs::exists(atlas_path, ec))
        {
            atlas_path = skel_path.parent_path() / (stem + ".atlas");
        }
        std::vector<char> skel_bytes;
        std::vector<char> atlas_bytes;
        if (!ReadAll(skel_path, skel_bytes) || !ReadAll(atlas_path, atlas_bytes))
        {
            continue;   // json 但不是骨架（没有同名图集）⇒ 不算资产
        }
        skel_bytes.push_back('\0');   // spine-cpp 的 json 读端要 C 串
        const size_t skel_len = skel_bytes.size() - 1;

        // 我们：解析 → 烘焙 → 写 → 读（= 运行时读 .zspg 的形态）
        SpineAtlas atlas;
        SpineSourceSkeleton src;
        SpineGpuAsset baked;
        std::vector<uint8_t> container;
        SpineGpuAsset asset;
        SpineImportResult r = ParseSpineAtlas(atlas_bytes.data(), atlas_bytes.size(), atlas);
        if (r.ok)
        {
            r = is_skel ? ParseSpineSkeletonBinary(reinterpret_cast<const uint8_t*>(skel_bytes.data()), skel_len, &atlas, src)
                        : ParseSpineSkeletonJson(skel_bytes.data(), skel_len, &atlas, src);
        }
        if (r.ok)
        {
            r = BakeSpineGpuAsset(src, baked);
        }
        if (r.ok)
        {
            r = WriteSpineGpuContainer(baked, container);
        }
        const long long live_before_asset = g_live_bytes;
        if (r.ok)
        {
            r = ReadSpineGpuContainer(container.data(), container.size(), asset);
        }
        const double container_bytes = static_cast<double>(g_live_bytes - live_before_asset);
        if (!r.ok || asset.anims.empty())
        {
            ++skipped;
            if (first_skip.empty())
            {
                first_skip = fname + ": " + (r.ok ? std::string("no animations") : r.error);
            }
            continue;
        }

        RefAttachmentLoader loader(&atlas);
        spine::SkeletonBinary bin(&loader);
        spine::SkeletonJson json(&loader);
        const long long live_before_ref = g_live_bytes;
        spine::SkeletonData* ref = is_skel ? bin.readSkeletonData(reinterpret_cast<const unsigned char*>(skel_bytes.data()),
                                                                  static_cast<int>(skel_len))
                                           : json.readSkeletonData(skel_bytes.data());
        if (ref == nullptr)
        {
            ++skipped;
            continue;
        }

        AssetResult ar;
        ar.ref_skeleton_data_bytes = static_cast<double>(g_live_bytes - live_before_ref);
        ar.ours_container_bytes = container_bytes;
        ar.name = skel_path.filename().string();
        ar.bones = asset.header.bone_count;
        ar.slots = asset.header.slot_count;
        ar.anims = static_cast<uint32_t>(asset.anims.size());
        ar.iks = static_cast<uint32_t>(asset.iks.size());
        ar.transforms = static_cast<uint32_t>(asset.transforms.size());
        ar.paths = static_cast<uint32_t>(asset.paths.size());
        ar.static_gpu_bytes = asset.vertex_buffer.size() + asset.influence_bind.size() * sizeof(float) +
                              asset.influence_bone.size() * sizeof(SpineInfluenceBone) +
                              asset.uv_buffer.size() * sizeof(float) + asset.index_buffer.size() * sizeof(uint16_t) +
                              asset.draw_items.size() * sizeof(SpineDrawItem);
        std::string err;
        // 先各跑一遍热身（指令 / 数据缓存、分配器），再正式计时；热身那趟顺带在末尾 15 帧
        // 数一遍堆分配（这时 vector 都已扩到稳态容量，数出来就是稳态每帧的分配次数）
        SideResult warm_ours, warm_ref;
        const bool ok_ours =
            RunOurs(asset, 30, warm_ours, err, 15, instances) && RunOurs(asset, frames, ar.ours, err, 0, instances);
        ar.ours.allocs_per_frame = warm_ours.allocs_per_frame;
        ar.ours.alloc_bytes_per_frame = warm_ours.alloc_bytes_per_frame;
        std::string err_ref;
        const bool ok_ref = ok_ours && RunRef(ref, 30, warm_ref, err_ref, 15, instances) &&
                            RunRef(ref, frames, ar.ref, err_ref, 0, instances);
        ar.ref.allocs_per_frame = warm_ref.allocs_per_frame;
        ar.ref.alloc_bytes_per_frame = warm_ref.alloc_bytes_per_frame;
        delete ref;
        if (!ok_ours || !ok_ref)
        {
            ++skipped;
            if (first_skip.empty())
            {
                first_skip = fname + ": " + (ok_ours ? err_ref : err);
            }
            continue;
        }
        results.push_back(std::move(ar));
    }
    const double wall_s = std::chrono::duration<double>(Clock::now() - t_all).count();

    if (results.empty())
    {
        std::printf("没有可测的资产（跳过 %zu：%s）\n", skipped, first_skip.c_str());
        return 1;
    }

    // ---- 汇总 ----
    const auto collect = [&](auto get) {
        std::vector<double> v;
        v.reserve(results.size());
        for (const AssetResult& a : results)
        {
            v.push_back(get(a));
        }
        return v;
    };
    const auto sum = [](const std::vector<double>& v) {
        double s = 0;
        for (double x : v)
        {
            s += x;
        }
        return s;
    };
    const auto row = [&](const char* label, auto get_ours, auto get_ref, const char* unit, double scale) {
        const std::vector<double> o = collect(get_ours);
        const std::vector<double> rf = collect(get_ref);
        const double so = sum(o), sr = sum(rf);
        std::printf("  %-14s  我们 中位 %9.2f  p90 %9.2f  合计 %11.1f | spine-cpp 中位 %9.2f  p90 %9.2f  合计 %11.1f %s | 比值(参考/我们) %.2fx\n",
                    label, Percentile(o, 0.5) * scale, Percentile(o, 0.9) * scale, so * scale,
                    Percentile(rf, 0.5) * scale, Percentile(rf, 0.9) * scale, sr * scale, unit, so > 0 ? sr / so : 0.0);
    };
    std::printf("=== GPU-Spine CPU 基准：%zu 个资产（跳过 %zu），每资产 %u 个实例 × %u 帧 @60Hz，墙钟 %.1f s ===\n",
                results.size(), skipped, instances, frames, wall_s);
    if (!first_skip.empty())
    {
        std::printf("  第一个跳过的：%s\n", first_skip.c_str());
    }
    std::printf("每实例每帧（µs；合计 = 每个资产各放 1 个实例时一帧的总成本；--instances N 时是 N 个实例的平均）：\n");
    row("动画 anim", [](const AssetResult& a) { return a.ours.anim_us; }, [](const AssetResult& a) { return a.ref.anim_us; },
        "µs", 1.0);
    if (split)
    {
        // Update = 推进轨道 / 排队事件（与资产规模无关的固定活）；Apply = 逐时间轴写状态（随动画条数长）。
        // 两边都是同一对入口，所以这两行能直接告诉我们"慢在哪半边"，不用猜。
        row("  · update", [](const AssetResult& a) { return a.ours.anim_update_us; },
            [](const AssetResult& a) { return a.ref.anim_update_us; }, "µs", 1.0);
        row("  · apply", [](const AssetResult& a) { return a.ours.anim_apply_us; },
            [](const AssetResult& a) { return a.ref.anim_apply_us; }, "µs", 1.0);
        row("  · 时间轴/帧", [](const AssetResult& a) { return a.ours.anim_timelines; },
            [](const AssetResult& a) { return a.ref.anim_timelines; }, "条", 1.0);
    }
    row("骨骼 pose", [](const AssetResult& a) { return a.ours.pose_us; }, [](const AssetResult& a) { return a.ref.pose_us; },
        "µs", 1.0);
    row("几何 geo", [](const AssetResult& a) { return a.ours.geo_us; }, [](const AssetResult& a) { return a.ref.geo_us; },
        "µs", 1.0);
    row("总计 total", [](const AssetResult& a) { return a.ours.total_us; },
        [](const AssetResult& a) { return a.ref.total_us; }, "µs", 1.0);
    std::printf("每实例每帧上传（KB）：\n");
    row("上传 upload", [](const AssetResult& a) { return a.ours.upload_bytes; },
        [](const AssetResult& a) { return a.ref.upload_bytes; }, "KB", 1.0 / 1024.0);
    row("CPU 顶点", [](const AssetResult& a) { return a.ours.vertices; }, [](const AssetResult& a) { return a.ref.vertices; },
        "个", 1.0);
    {
        // 稳态每帧堆分配：这行不是"谁快谁慢"，而是**谁在热路径里还有多余的活**。
        // 两边都应该是 0（或极小的常数）；我们这边只要它不是 0，就等于每帧在 malloc/free 上白烧钱。
        const std::vector<double> o = collect([](const AssetResult& a) { return a.ours.allocs_per_frame; });
        const std::vector<double> rf = collect([](const AssetResult& a) { return a.ref.allocs_per_frame; });
        const std::vector<double> ob = collect([](const AssetResult& a) { return a.ours.alloc_bytes_per_frame; });
        const std::vector<double> rfb = collect([](const AssetResult& a) { return a.ref.alloc_bytes_per_frame; });
        std::printf("稳态每帧堆分配（理想：两边都是 0）：\n");
        std::printf("  %-14s  我们 中位 %9.1f  p90 %9.1f  合计 %11.0f | spine-cpp 中位 %9.1f  p90 %9.1f  合计 %11.0f 次\n",
                    "分配次数", Percentile(o, 0.5), Percentile(o, 0.9), sum(o), Percentile(rf, 0.5), Percentile(rf, 0.9),
                    sum(rf));
        std::printf("  %-14s  我们 中位 %9.1f  p90 %9.1f  合计 %11.0f | spine-cpp 中位 %9.1f  p90 %9.1f  合计 %11.0f B\n",
                    "分配字节", Percentile(ob, 0.5), Percentile(ob, 0.9), sum(ob), Percentile(rfb, 0.5), Percentile(rfb, 0.9),
                    sum(rfb));
    }
    {
        const std::vector<double> st = collect([](const AssetResult& a) { return static_cast<double>(a.static_gpu_bytes); });
        std::printf("我们的静态显存（一次性上传，同资产实例共享）：中位 %.1f KB  p90 %.1f KB  合计 %.1f MB\n",
                    Percentile(st, 0.5) / 1024.0, Percentile(st, 0.9) / 1024.0, sum(st) / (1024.0 * 1024.0));
    }
    {
        // ---- 内存：共享（每资产一份） vs 每实例（稳态常驻）----
        std::printf("内存（KB，堆常驻；共享 = 每资产一份，每实例 = 每多一个实例多出来的）：\n");
        row("CPU 共享", [](const AssetResult& a) { return a.ours_container_bytes + a.ours.shared_bytes; },
            [](const AssetResult& a) { return a.ref_skeleton_data_bytes + a.ref.shared_bytes; }, "KB", 1.0 / 1024.0);
        row("CPU 每实例", [](const AssetResult& a) { return a.ours.per_instance_bytes; },
            [](const AssetResult& a) { return a.ref.per_instance_bytes; }, "KB", 1.0 / 1024.0);
        // GPU：我们 = 静态（共享）+ 每帧上传那份（每实例，要乘 frames-in-flight）；
        //      spine-cpp = 没有静态，每实例一份动态 VB/IB（同样乘 frames-in-flight）
        row("GPU 共享", [](const AssetResult& a) { return static_cast<double>(a.static_gpu_bytes); },
            [](const AssetResult&) { return 0.0; }, "KB", 1.0 / 1024.0);
        row("GPU 每实例/帧", [](const AssetResult& a) { return a.ours.upload_bytes; },
            [](const AssetResult& a) { return a.ref.upload_bytes; }, "KB", 1.0 / 1024.0);
        // 同一个资产放 N 个实例的总内存（CPU + GPU，GPU 动态部分按 3 帧在途）：平均资产
        const double cnt = static_cast<double>(results.size());
        const double o_sh = sum(collect([](const AssetResult& a) {
                                return a.ours_container_bytes + a.ours.shared_bytes + static_cast<double>(a.static_gpu_bytes);
                            })) / cnt;
        const double r_sh =
            sum(collect([](const AssetResult& a) { return a.ref_skeleton_data_bytes + a.ref.shared_bytes; })) / cnt;
        const double o_pi =
            sum(collect([](const AssetResult& a) { return a.ours.per_instance_bytes + 3.0 * a.ours.upload_bytes; })) / cnt;
        const double r_pi =
            sum(collect([](const AssetResult& a) { return a.ref.per_instance_bytes + 3.0 * a.ref.upload_bytes; })) / cnt;
        std::printf("平均资产放 N 个实例的总内存（CPU 堆 + GPU，动态部分 ×3 帧在途）：\n");
        for (const int n : {1, 10, 100})
        {
            const double o = o_sh + n * o_pi;
            const double rr = r_sh + n * r_pi;
            std::printf("  N=%-4d 我们 %9.1f KB | spine-cpp %9.1f KB | 比值(参考/我们) %.2fx\n", n, o / 1024.0, rr / 1024.0,
                        o > 0 ? rr / o : 0.0);
        }
        // 两条直线 o_sh + N·o_pi 与 r_sh + N·r_pi 的交点：只有"一边共享大、另一边每实例大"时才有意义
        if (o_sh > r_sh && o_pi < r_pi)
        {
            std::printf("  ⇒ 我们共享部分更大、每实例更小：N ≥ %.1f 时我们总内存更少\n", (o_sh - r_sh) / (r_pi - o_pi));
        }
        else if (o_sh <= r_sh && o_pi <= r_pi)
        {
            std::printf("  ⇒ 共享与每实例两项我们都不大于参考：任何 N 都是我们总内存更少\n");
        }
        else if (o_sh < r_sh && o_pi > r_pi)
        {
            std::printf("  ⇒ 我们共享部分更小、每实例更大：N ≥ %.1f 时参考总内存更少\n", (r_sh - o_sh) / (o_pi - r_pi));
        }
        else
        {
            std::printf("  ⇒ 共享与每实例两项我们都更大：任何 N 都是参考总内存更少\n");
        }
    }
    {
        // 1 ms 预算能放多少个实例（单线程，按合计 / 资产数的平均实例算）
        const double mo = sum(collect([](const AssetResult& a) { return a.ours.total_us; })) / results.size();
        const double mr = sum(collect([](const AssetResult& a) { return a.ref.total_us; })) / results.size();
        const double uo = sum(collect([](const AssetResult& a) { return a.ours.upload_bytes; })) / results.size();
        const double ur = sum(collect([](const AssetResult& a) { return a.ref.upload_bytes; })) / results.size();
        std::printf("平均实例：我们 %.2f µs + %.1f KB/帧，spine-cpp %.2f µs + %.1f KB/帧 ⇒ 1 ms 单线程预算 ≈ 我们 %.0f 个 / spine-cpp %.0f 个；"
                    "100 个实例 @60Hz 上传 ≈ 我们 %.1f MB/s / spine-cpp %.1f MB/s\n",
                    mo, uo / 1024.0, mr, ur / 1024.0, 1000.0 / mo, 1000.0 / mr, uo * 100 * 60 / (1024.0 * 1024.0),
                    ur * 100 * 60 / (1024.0 * 1024.0));
    }
    // 最重的几个（按 spine-cpp 总成本）
    std::vector<const AssetResult*> sorted;
    for (const AssetResult& a : results)
    {
        sorted.push_back(&a);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const AssetResult* a, const AssetResult* b) { return a->ref.total_us > b->ref.total_us; });
    std::printf("最重的 %zu 个（按 spine-cpp 总成本）：\n", std::min(top, sorted.size()));
    for (size_t i = 0; i < std::min(top, sorted.size()); ++i)
    {
        const AssetResult& a = *sorted[i];
        std::printf("  %-40s 骨 %3u 槽 %3u ik/tf/path %u/%u/%u | 我们 %7.2f µs（pose %6.2f）%6.1f KB | spine-cpp %7.2f µs（pose %5.2f）%6.1f KB（%5.0f 顶点）\n",
                    a.name.substr(0, 40).c_str(), a.bones, a.slots, a.iks, a.transforms, a.paths, a.ours.total_us,
                    a.ours.pose_us, a.ours.upload_bytes / 1024.0, a.ref.total_us, a.ref.pose_us, a.ref.upload_bytes / 1024.0,
                    a.ref.vertices);
    }
    if (!csv_path.empty())
    {
        std::ofstream csv(csv_path, std::ios::trunc);
        csv << "asset,bones,slots,anims,iks,transforms,paths,static_gpu_bytes,ours_anim_us,ours_pose_us,ours_geo_us,ours_total_us,ours_upload_bytes,"
               "ours_cpu_verts,ours_allocs,ours_alloc_bytes,ours_anim_update_us,ours_anim_apply_us,ours_timelines,ref_anim_us,ref_pose_us,ref_geo_us,"
               "ref_total_us,ref_upload_bytes,ref_verts,ref_allocs,ref_alloc_bytes,ref_anim_update_us,ref_anim_apply_us,ref_timelines,"
               "ours_container_bytes,ours_shared_bytes,ours_per_instance_bytes,ref_skeleton_data_bytes,ref_shared_bytes,ref_per_instance_bytes\n";
        for (const AssetResult& a : results)
        {
            csv << a.name << ',' << a.bones << ',' << a.slots << ',' << a.anims << ',' << a.iks << ',' << a.transforms
                << ',' << a.paths << ',' << a.static_gpu_bytes << ','
                << a.ours.anim_us << ',' << a.ours.pose_us << ',' << a.ours.geo_us << ',' << a.ours.total_us << ','
                << a.ours.upload_bytes << ',' << a.ours.vertices << ',' << a.ours.allocs_per_frame << ','
                << a.ours.alloc_bytes_per_frame << ',' << a.ours.anim_update_us << ',' << a.ours.anim_apply_us << ','
                << a.ours.anim_timelines << ',' << a.ref.anim_us << ',' << a.ref.pose_us << ','
                << a.ref.geo_us << ',' << a.ref.total_us << ',' << a.ref.upload_bytes << ',' << a.ref.vertices << ','
                << a.ref.allocs_per_frame << ',' << a.ref.alloc_bytes_per_frame << ',' << a.ref.anim_update_us << ','
                << a.ref.anim_apply_us << ',' << a.ref.anim_timelines << ',' << a.ours_container_bytes << ','
                << a.ours.shared_bytes << ',' << a.ours.per_instance_bytes << ',' << a.ref_skeleton_data_bytes << ','
                << a.ref.shared_bytes << ',' << a.ref.per_instance_bytes << '\n';
        }
        std::printf("逐资产明细：%s\n", csv_path.c_str());
    }
    return 0;
}
