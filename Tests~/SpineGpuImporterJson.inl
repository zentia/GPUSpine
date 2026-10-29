// =====================================================================
// 【仅测试用】Spine 3.8 json 骨架解析器
// ---------------------------------------------------------------------
// 产品代码（GPUSpine 核心）只支持二进制 .skel，不依赖 rapidjson。
// 这份 json 解析器只服务于测试夹具（手写 json 比手写二进制好读，且用于 json<->skel 对拍），
// 由 SpineGpuImporter.cpp 末尾在定义了 GPUSPINE_ENABLE_TEST_JSON 时 #include 进来，
// 与核心共用同一个翻译单元，所以能直接用核心匿名命名空间里的帮助函数。
// 声明见同目录 SpineGpuJsonFixture.h。
// =====================================================================

#include <rapidjson/document.h>

namespace
{

using JsonValue = rapidjson::Value;

bool TryGetMember(const JsonValue& v, const char* key, JsonValue::ConstMemberIterator& out)
{
    if (!v.IsObject())
    {
        return false;
    }
    out = v.FindMember(key);
    return out != v.MemberEnd();
}

const JsonValue* GetMember(const JsonValue& v, const char* key)
{
    if (!v.IsObject())
    {
        return nullptr;
    }
    auto it = v.FindMember(key);
    return it != v.MemberEnd() ? &it->value : nullptr;
}

float GetFloat(const JsonValue& v, const char* key, float fallback)
{
    const JsonValue* m = GetMember(v, key);
    return (m && m->IsNumber()) ? m->GetFloat() : fallback;
}

std::string GetString(const JsonValue& v, const char* key, const char* fallback)
{
    const JsonValue* m = GetMember(v, key);
    return (m && m->IsString()) ? std::string(m->GetString()) : std::string(fallback);
}

// 与参考实现的 Json::getInt(..., fallback) 对齐（Json.cpp:89-91 读的是 `_valueInt`）。
//
// ⚠️ **布尔必须当 0/1**：spine 自己的 json 解析器把 true / false 写进同一个
//    `_valueInt`（Json.cpp:165 的 calloc ⇒ false = 0，:173 ⇒ true = 1），
//    而 Spine 编辑器导出的 json 里 `compress` / `stretch` / `uniform` /
//    `local` / `relative` / `bendPositive` / `constantSpeed` **全都是布尔**。
//    这里若只认 IsNumber()，布尔会被当成"非数字"回落到 fallback —— 不报错、
//    值悄悄反了。实测踩到过一次：夹具写 `"bendPositive": false` 而 IK 拿到
//    bend_dir = +1、`"constantSpeed": false` 而 path 附件拿到 constantSpeed = 1，
//    只有 skel↔json 逐字段对拍抓得到（二进制那边是显式布尔，不会错）。
//    用到它的地方（linked mesh 的 "deform"）在 spine 里也是 0/1 的开关。
int GetInt(const JsonValue& v, const char* key, int fallback)
{
    const JsonValue* m = GetMember(v, key);
    if (m == nullptr)
    {
        return fallback;
    }
    if (m->IsBool())
    {
        return m->GetBool() ? 1 : 0;
    }
    if (m->IsInt())
    {
        return m->GetInt();
    }
    return m->IsNumber() ? static_cast<int>(m->GetFloat()) : fallback;
}

// 槽位名 → 下标。参考实现里到处是 `skeletonData->findSlot(name)`，而它返回 NULL 后
// 调用方常常不检查（deform 的 :1051 空指针解引用、clip 的 :634 静默变 NULL）。
// 这里统一走这个函数，找不到就**报错**，不让作用域/目标悄悄变。
int FindSlotIndex(const std::vector<SpineSourceSlot>& slots, const std::string& name)
{
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].name == name)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}


// =====================================================================
// 动画：json（SkeletonJson.cpp:758-1190）
// ---------------------------------------------------------------------
// 抄的时候最需要注意的不是字段，而是**三类"参考实现静默、我们报错"**：
//   1. 约束名找不到（`findIkConstraint()` 返回 NULL 后 `if (constraint == ...)` 永不成立
//      ⇒ 下标**留在 0**，SkeletonJson.cpp:933-940）⇒ 动到了**另一条**约束且不报错。
//   2. `readCurve()` 认不出的曲线字节（switch 没有 default，SkeletonBinary.cpp:1112）。
//   3. color 串长度不足 8（`toColor()` 返回 **-1**，:724-725）⇒ 颜色变成负数，
//      而"某个槽位颜色不对"不会有任何报错。
// =====================================================================

// 逐字符抄 SkeletonJson::toColor（:719-739）："rrggbbaa" 里第 index 个字节。
// ⚠️ 长度不足时返回 **-1** 而不是 0 —— 照抄（改了就与参考实现分叉）。
float ToColorComponent(const std::string& value, size_t index)
{
    if (index >= value.size() / 2)
    {
        return -1.0f;
    }
    const char hi = value[index * 2];
    const char lo = value[index * 2 + 1];
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    };
    const int h = hex(hi);
    const int l = hex(lo);
    if (h < 0 || l < 0)
    {
        return -1.0f;
    }
    return static_cast<float>(h * 16 + l) / 255.0f;
}

// 一个动画里的一个时间轴段：帧数组 + 每帧的曲线。
// `readCurve(valueMap, timeline, frameIndex)` 在**每一帧**都调用（含最后一帧），
// 而 CurveTimeline 只有 frameCount-1 段 —— 参考实现靠 `assert` 挡住越界（Release 下
// 就是越界写）。我们显式只收前 frameCount-1 段。
bool ReadCurveJson(const JsonValue& frame, SpineSourceCurve& out, std::string& err)
{
    out = SpineSourceCurve {};
    const JsonValue* curve = GetMember(frame, "curve");
    if (curve == nullptr)
    {
        return true;   // 没有 "curve" ⇒ linear（:741-742）
    }
    if (curve->IsString())
    {
        if (std::string(curve->GetString()) == "stepped")
        {
            out.type = SpineCurveType::kStepped;
            return true;
        }
        err = "曲线串 '" + std::string(curve->GetString()) + "' 不认识（只认 \"stepped\"）";
        return false;
    }
    if (curve->IsArray() && curve->Size() == 4)
    {
        out.type = SpineCurveType::kBezier;
        out.cx1 = static_cast<float>((*curve)[0].GetDouble());
        out.cy1 = static_cast<float>((*curve)[1].GetDouble());
        out.cx2 = static_cast<float>((*curve)[2].GetDouble());
        out.cy2 = static_cast<float>((*curve)[3].GetDouble());
        return true;
    }
    // ⚠️ 参考实现在这里**什么都不做**（4 个 if 之后没有 else）⇒ 静默变 linear。
    err = "curve 既不是 \"stepped\" 也不是 4 元数组（参考实现在这里静默退回 linear）";
    return false;
}

// 往 frames 里追加一帧，并把曲线收进去。`values` 的个数必须等于
// SpineTimelineEntries(type) - 1（time 另算）。
bool PushFrame(SpineSourceTimeline& tl, const JsonValue& frame, const float* values, uint32_t value_count,
               std::string& err)
{
    if (value_count + 1u != SpineTimelineEntries(tl.type))
    {
        err = "时间轴内部错误：entries 不匹配";
        return false;
    }
    tl.frames.push_back(GetFloat(frame, "time", 0.0f));
    for (uint32_t i = 0; i < value_count; ++i)
    {
        tl.frames.push_back(values[i]);
    }
    return true;
}

SpineImportResult ParseSpineAnimationJson(const JsonValue& anim, const std::string& anim_name,
                                          const SpineSourceSkeleton& skel, SpineSourceAnimation& out)
{
    const auto fail = [](const std::string& m) { return Fail("json: animations." + m); };

    // ---- bones：rotate / translate / scale / shear ----
    if (const JsonValue* bones = GetMember(anim, "bones"))
    {
        if (!bones->IsObject())
        {
            return fail(anim_name + ".bones 不是对象");
        }
        for (auto bone_it = bones->MemberBegin(); bone_it != bones->MemberEnd(); ++bone_it)
        {
            const std::string bone_name(bone_it->name.GetString());
            int bone_index = -1;
            for (size_t i = 0; i < skel.bones.size(); ++i)
            {
                if (skel.bones[i].name == bone_name)
                {
                    bone_index = static_cast<int>(i);
                    break;
                }
            }
            // 参考实现 `findBoneIndex()` 返回 -1 后就拿 -1 当骨骼下标用（:875-885 之间），
            // 之后是越界读 ⇒ 报错。
            if (bone_index < 0)
            {
                return fail(anim_name + ".bones 引用了不存在的骨骼 '" + bone_name + "'");
            }
            if (!bone_it->value.IsObject())
            {
                return fail(anim_name + ".bones." + bone_name + " 不是对象");
            }
            for (auto tl_it = bone_it->value.MemberBegin(); tl_it != bone_it->value.MemberEnd(); ++tl_it)
            {
                const std::string kind(tl_it->name.GetString());
                SpineTimelineType type;
                if (kind == "rotate")
                {
                    type = SpineTimelineType::kRotate;
                }
                else if (kind == "translate")
                {
                    type = SpineTimelineType::kTranslate;
                }
                else if (kind == "scale")
                {
                    type = SpineTimelineType::kScale;
                }
                else if (kind == "shear")
                {
                    type = SpineTimelineType::kShear;
                }
                else
                {
                    // 参考实现在这里报 "Invalid timeline type for a bone"（:931）。
                    return fail(anim_name + ".bones." + bone_name + " 的时间轴类型 '" + kind +
                                "' 不认识（参考实现报 Invalid timeline type for a bone）");
                }
                if (!tl_it->value.IsArray())
                {
                    return fail(anim_name + ".bones." + bone_name + "." + kind + " 不是数组");
                }

                SpineSourceTimeline tl;
                tl.type = type;
                tl.target = bone_index;
                std::string err;
                for (const JsonValue& frame : tl_it->value.GetArray())
                {
                    if (!frame.IsObject())
                    {
                        return fail(anim_name + ".bones." + bone_name + "." + kind + " 的帧不是对象");
                    }
                    if (type == SpineTimelineType::kRotate)
                    {
                        // ⚠️ 字段名是 **"angle"**（SkeletonJson.cpp:895），不是 "rotation"/"value"
                        const float v[1] = { GetFloat(frame, "angle", 0.0f) };
                        if (!PushFrame(tl, frame, v, 1u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    else
                    {
                        // translate / scale / shear 都是 (x, y)；timelineScale = 1（_scale 恒为 1）
                        const float v[2] = { GetFloat(frame, "x", 0.0f), GetFloat(frame, "y", 0.0f) };
                        if (!PushFrame(tl, frame, v, 2u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    SpineSourceCurve c;
                    if (!ReadCurveJson(frame, c, err))
                    {
                        return fail(anim_name + ".bones." + bone_name + "." + kind + ": " + err);
                    }
                    tl.curves.push_back(c);
                }
                TrimCurves(tl);
                out.timelines.push_back(std::move(tl));
            }
        }
    }

    // ---- slots：attachment / color / twoColor ----
    if (const JsonValue* slots = GetMember(anim, "slots"))
    {
        if (!slots->IsObject())
        {
            return fail(anim_name + ".slots 不是对象");
        }
        for (auto slot_it = slots->MemberBegin(); slot_it != slots->MemberEnd(); ++slot_it)
        {
            const std::string slot_name(slot_it->name.GetString());
            const int slot_index = FindSlotIndex(skel.slots, slot_name);
            if (slot_index < 0)
            {
                return fail(anim_name + ".slots 引用了不存在的槽位 '" + slot_name +
                            "'（参考实现报 Slot not found）");
            }
            if (!slot_it->value.IsObject())
            {
                return fail(anim_name + ".slots." + slot_name + " 不是对象");
            }
            for (auto tl_it = slot_it->value.MemberBegin(); tl_it != slot_it->value.MemberEnd(); ++tl_it)
            {
                const std::string kind(tl_it->name.GetString());
                if (!tl_it->value.IsArray())
                {
                    return fail(anim_name + ".slots." + slot_name + "." + kind + " 不是数组");
                }
                SpineSourceTimeline tl;
                std::string err;
                if (kind == "attachment")
                {
                    tl.type = SpineTimelineType::kAttachment;
                    tl.target = slot_index;
                    for (const JsonValue& frame : tl_it->value.GetArray())
                    {
                        tl.frames.push_back(GetFloat(frame, "time", 0.0f));
                        // `"name": null` ⇒ 空串 = 该帧**隐藏**这个槽位（:821-823）。
                        const JsonValue* n = GetMember(frame, "name");
                        tl.attachment_names.push_back((n != nullptr && n->IsString())
                                                          ? std::string(n->GetString())
                                                          : std::string());
                    }
                    TrimCurves(tl);
                }
                else if (kind == "color")
                {
                    tl.type = SpineTimelineType::kColor;
                    tl.target = slot_index;
                    for (const JsonValue& frame : tl_it->value.GetArray())
                    {
                        const std::string col = GetString(frame, "color", "");
                        const float v[4] = {
                            ToColorComponent(col, 0), ToColorComponent(col, 1),
                            ToColorComponent(col, 2), ToColorComponent(col, 3),
                        };
                        if (!PushFrame(tl, frame, v, 4u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                        SpineSourceCurve c;
                        if (!ReadCurveJson(frame, c, err))
                        {
                            return fail(anim_name + ".slots." + slot_name + ".color: " + err);
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                }
                else if (kind == "twoColor")
                {
                    tl.type = SpineTimelineType::kTwoColor;
                    tl.target = slot_index;
                    for (const JsonValue& frame : tl_it->value.GetArray())
                    {
                        const std::string light = GetString(frame, "light", "");
                        const std::string dark = GetString(frame, "dark", "");
                        // ⚠️ dark 只有 **rgb** 三个分量（:863-866）⇒ 加起来是 1 + 4 + 3 = 8
                        const float v[7] = {
                            ToColorComponent(light, 0), ToColorComponent(light, 1),
                            ToColorComponent(light, 2), ToColorComponent(light, 3),
                            ToColorComponent(dark, 0),  ToColorComponent(dark, 1),
                            ToColorComponent(dark, 2),
                        };
                        if (!PushFrame(tl, frame, v, 7u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                        SpineSourceCurve c;
                        if (!ReadCurveJson(frame, c, err))
                        {
                            return fail(anim_name + ".slots." + slot_name + ".twoColor: " + err);
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                }
                else
                {
                    return fail(anim_name + ".slots." + slot_name + " 的时间轴类型 '" + kind +
                                "' 不认识（只认 attachment / color / twoColor）");
                }
                out.timelines.push_back(std::move(tl));
            }
        }
    }

    // ---- ik / transform：值是"约束名 → 帧数组"的**对象** ----
    const auto read_mix_timeline = [&](const JsonValue& holder, const char* section,
                                       const std::vector<std::string>& names, SpineTimelineType type,
                                       uint32_t value_count, const char* const* keys) -> SpineImportResult {
        if (!holder.IsObject())
        {
            return fail(anim_name + "." + section + " 不是对象");
        }
        for (auto it = holder.MemberBegin(); it != holder.MemberEnd(); ++it)
        {
            const std::string cname(it->name.GetString());
            int index = -1;
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (names[i] == cname)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            // ⚠️ 参考实现 `findIkConstraint()` 返回 NULL 后找下标的循环一次都不命中 ⇒
            //    `_ikConstraintIndex` **留在 0**（SkeletonJson.cpp:933-940）⇒ 动到第一条
            //    约束，而"动画动错了约束"不报错。path 那条却有检查（:990-994）。
            if (index < 0)
            {
                return fail(anim_name + "." + section + " 引用了不存在的约束 '" + cname +
                            "'（参考实现在这里把下标静默留在 0）");
            }
            if (!it->value.IsArray())
            {
                return fail(anim_name + "." + section + "." + cname + " 不是数组");
            }
            SpineSourceTimeline tl;
            tl.type = type;
            tl.target = index;
            std::string err;
            for (const JsonValue& frame : it->value.GetArray())
            {
                float v[5] = { 0, 0, 0, 0, 0 };
                for (uint32_t k = 0; k < value_count; ++k)
                {
                    v[k] = GetFloat(frame, keys[k], 1.0f);
                }
                if (!PushFrame(tl, frame, v, value_count, err))
                {
                    return fail(anim_name + ": " + err);
                }
                SpineSourceCurve c;
                if (!ReadCurveJson(frame, c, err))
                {
                    return fail(anim_name + "." + section + "." + cname + ": " + err);
                }
                tl.curves.push_back(c);
            }
            TrimCurves(tl);
            out.timelines.push_back(std::move(tl));
        }
        return Ok();
    };

    if (const JsonValue* ik = GetMember(anim, "ik"))
    {
        // ⚠️ IK 帧里 bendPositive / compress / stretch 在 json 里是 **0/1 数字**
        //    （`Json::getInt(valueMap, "bendPositive", 1) ? 1 : -1`，:945-947），
        //    而容器里存的是 float ⇒ 1 / -1 / 0。
        if (!ik->IsObject())
        {
            return fail(anim_name + ".ik 不是对象");
        }
        for (auto it = ik->MemberBegin(); it != ik->MemberEnd(); ++it)
        {
            const std::string cname(it->name.GetString());
            int index = -1;
            for (size_t i = 0; i < skel.ik_names.size(); ++i)
            {
                if (skel.ik_names[i] == cname)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0)
            {
                return fail(anim_name + ".ik 引用了不存在的约束 '" + cname +
                            "'（参考实现在这里把下标静默留在 0）");
            }
            if (!it->value.IsArray())
            {
                return fail(anim_name + ".ik." + cname + " 不是数组");
            }
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kIkConstraint;
            tl.target = index;
            std::string err;
            for (const JsonValue& frame : it->value.GetArray())
            {
                const float bend = GetInt(frame, "bendPositive", 1) ? 1.0f : -1.0f;
                const float v[4] = {
                    GetFloat(frame, "mix", 1.0f),
                    bend,
                    GetInt(frame, "compress", 0) ? 1.0f : 0.0f,
                    GetInt(frame, "stretch", 0) ? 1.0f : 0.0f,
                };
                if (!PushFrame(tl, frame, v, 4u, err))
                {
                    return fail(anim_name + ": " + err);
                }
                SpineSourceCurve c;
                if (!ReadCurveJson(frame, c, err))
                {
                    return fail(anim_name + ".ik." + cname + ": " + err);
                }
                tl.curves.push_back(c);
            }
            TrimCurves(tl);
            out.timelines.push_back(std::move(tl));
        }
    }

    if (const JsonValue* tc = GetMember(anim, "transform"))
    {
        static const char* kTransformKeys[4] = { "rotateMix", "translateMix", "scaleMix", "shearMix" };
        const SpineImportResult r =
            read_mix_timeline(*tc, "transform", skel.transform_names,
                              SpineTimelineType::kTransformConstraint, 4u, kTransformKeys);
        if (!r.ok)
        {
            return r;
        }
    }

    // ---- paths：**两**层（约束名 → 时间轴名 → 帧数组），与 ik/transform 不同 ----
    if (const JsonValue* paths = GetMember(anim, "paths"))
    {
        if (!paths->IsObject())
        {
            return fail(anim_name + ".paths 不是对象");
        }
        for (auto c_it = paths->MemberBegin(); c_it != paths->MemberEnd(); ++c_it)
        {
            const std::string cname(c_it->name.GetString());
            int index = -1;
            for (size_t i = 0; i < skel.path_names.size(); ++i)
            {
                if (skel.path_names[i] == cname)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0)
            {
                return fail(anim_name + ".paths 引用了不存在的约束 '" + cname +
                            "'（参考实现报 Path constraint not found）");
            }
            if (!c_it->value.IsObject())
            {
                return fail(anim_name + ".paths." + cname + " 不是对象");
            }
            for (auto tl_it = c_it->value.MemberBegin(); tl_it != c_it->value.MemberEnd(); ++tl_it)
            {
                const std::string kind(tl_it->name.GetString());
                SpineTimelineType type;
                const char* value_key = nullptr;
                if (kind == "position")
                {
                    type = SpineTimelineType::kPathConstraintPosition;
                    value_key = "position";
                }
                else if (kind == "spacing")
                {
                    type = SpineTimelineType::kPathConstraintSpacing;
                    value_key = "spacing";
                }
                else if (kind == "mix")
                {
                    type = SpineTimelineType::kPathConstraintMix;
                }
                else
                {
                    return fail(anim_name + ".paths." + cname + " 的时间轴类型 '" + kind +
                                "' 不认识（只认 position / spacing / mix）");
                }
                if (!tl_it->value.IsArray())
                {
                    return fail(anim_name + ".paths." + cname + "." + kind + " 不是数组");
                }
                SpineSourceTimeline tl;
                tl.type = type;
                tl.target = index;
                std::string err;
                for (const JsonValue& frame : tl_it->value.GetArray())
                {
                    if (type == SpineTimelineType::kPathConstraintMix)
                    {
                        const float v[2] = { GetFloat(frame, "rotateMix", 1.0f),
                                             GetFloat(frame, "translateMix", 1.0f) };
                        if (!PushFrame(tl, frame, v, 2u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    else
                    {
                        const float v[1] = { GetFloat(frame, value_key, 0.0f) };
                        if (!PushFrame(tl, frame, v, 1u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                    }
                    SpineSourceCurve c;
                    if (!ReadCurveJson(frame, c, err))
                    {
                        return fail(anim_name + ".paths." + cname + "." + kind + ": " + err);
                    }
                    tl.curves.push_back(c);
                }
                TrimCurves(tl);
                out.timelines.push_back(std::move(tl));
            }
        }
    }

    // ---- deform：皮肤 → 槽位 → 附件 → 帧数组（**四**层）----
    if (const JsonValue* deform = GetMember(anim, "deform"))
    {
        if (!deform->IsObject())
        {
            return fail(anim_name + ".deform 不是对象");
        }
        for (auto sit = deform->MemberBegin(); sit != deform->MemberEnd(); ++sit)
        {
            const std::string skin_name(sit->name.GetString());
            if (!sit->value.IsObject())
            {
                return fail(anim_name + ".deform 里皮肤 '" + skin_name + "' 的值不是对象");
            }
            int skin_index = -1;
            for (size_t i = 0; i < skel.skins.size(); ++i)
            {
                if (skel.skins[i] == skin_name)
                {
                    skin_index = static_cast<int>(i);
                    break;
                }
            }
            // 参考实现 `findSkin(...)` 返回 NULL 后紧接着 `skin->getAttachment(...)`
            // ⇒ 皮肤名写错是**空指针**，不是报错。
            if (skin_index < 0)
            {
                return fail(anim_name + ".deform 引用了不存在的皮肤 '" + skin_name +
                            "'（参考实现在这里是空指针解引用）");
            }
            for (auto slot_it = sit->value.MemberBegin(); slot_it != sit->value.MemberEnd(); ++slot_it)
            {
                const std::string slot_name(slot_it->name.GetString());
                if (!slot_it->value.IsObject())
                {
                    return fail(anim_name + ".deform 里槽位 '" + slot_name + "' 的值不是对象");
                }
                const int slot_index = FindSlotIndex(skel.slots, slot_name);
                if (slot_index < 0)
                {
                    return fail(anim_name + ".deform 引用了不存在的槽位 '" + slot_name + "'");
                }
                for (auto att_it = slot_it->value.MemberBegin(); att_it != slot_it->value.MemberEnd();
                     ++att_it)
                {
                    const std::string att_name(att_it->name.GetString());
                    if (!att_it->value.IsArray())
                    {
                        return fail(anim_name + ".deform 里附件 '" + att_name + "' 的值不是帧数组");
                    }
                    SpineSourceTimeline tl;
                    tl.type = SpineTimelineType::kDeform;
                    tl.deform_skin = skin_index;
                    tl.deform_slot = slot_index;
                    tl.deform_attachment = att_name;
                    std::string err;
                    for (const JsonValue& frame : att_it->value.GetArray())
                    {
                        SpineSourceDeformFrame df;
                        const JsonValue* verts = GetMember(frame, "vertices");
                        if (verts != nullptr)
                        {
                            if (!verts->IsArray())
                            {
                                return fail(anim_name + ".deform." + att_name + " 的 vertices 不是数组");
                            }
                            const int offset = static_cast<int>(GetFloat(frame, "offset", 0.0f));
                            if (offset < 0)
                            {
                                return fail(anim_name + ".deform." + att_name + " 的帧 offset " +
                                            std::to_string(offset) + " 为负");
                            }
                            df.start = static_cast<uint32_t>(offset);
                            for (const JsonValue& v : verts->GetArray())
                            {
                                df.values.push_back(static_cast<float>(v.GetDouble()));
                            }
                        }
                        // verts == nullptr ⇒ 回退帧（df.values 空）
                        if (!PushFrame(tl, frame, nullptr, 0u, err))
                        {
                            return fail(anim_name + ": " + err);
                        }
                        tl.deform_frames.push_back(std::move(df));
                        SpineSourceCurve c;
                        if (!ReadCurveJson(frame, c, err))
                        {
                            return fail(anim_name + ".deform." + att_name + ": " + err);
                        }
                        tl.curves.push_back(c);
                    }
                    TrimCurves(tl);
                    out.timelines.push_back(std::move(tl));
                }
            }
        }
    }

    // ---- drawOrder / draworder：整个骨架的槽位顺序 ----
    const JsonValue* draw_order = GetMember(anim, "drawOrder");
    if (draw_order == nullptr)
    {
        draw_order = GetMember(anim, "draworder");   // 参考实现两个都认（:772-774）
    }
    if (draw_order != nullptr)
    {
        if (!draw_order->IsArray())
        {
            return fail(anim_name + ".drawOrder 不是数组");
        }
        SpineSourceTimeline tl;
        tl.type = SpineTimelineType::kDrawOrder;
        tl.target = 0;
        for (const JsonValue& frame : draw_order->GetArray())
        {
            SpineSourceDrawOrderFrame dof;
            if (const JsonValue* offsets = GetMember(frame, "offsets"))
            {
                if (!offsets->IsArray())
                {
                    return fail(anim_name + ".drawOrder 的 offsets 不是数组");
                }
                for (const JsonValue& o : offsets->GetArray())
                {
                    const std::string sname = GetString(o, "slot", "");
                    const int si = FindSlotIndex(skel.slots, sname);
                    if (si < 0)
                    {
                        return fail(anim_name + ".drawOrder 引用了不存在的槽位 '" + sname +
                                    "'（参考实现报 Slot not found）");
                    }
                    dof.slots.push_back(static_cast<uint32_t>(si));
                    dof.offsets.push_back(GetInt(o, "offset", 0));
                }
            }
            tl.frames.push_back(GetFloat(frame, "time", 0.0f));
            tl.draw_order_frames.push_back(std::move(dof));
        }
        TrimCurves(tl);
        out.timelines.push_back(std::move(tl));
    }

    // ---- events（v16，SkeletonJson.cpp:1172-1198）----
    if (const JsonValue* events = GetMember(anim, "events"))
    {
        if (!events->IsArray())
        {
            return fail(anim_name + ".events 不是数组");
        }
        if (events->Size() > 0)
        {
            SpineSourceTimeline tl;
            tl.type = SpineTimelineType::kEvent;
            tl.target = 0;
            for (const JsonValue& frame : events->GetArray())
            {
                const std::string ename = GetString(frame, "name", "");
                int ei = -1;
                for (size_t k = 0; k < skel.events.size(); ++k)
                {
                    if (skel.events[k].name == ename)
                    {
                        ei = static_cast<int>(k);
                        break;
                    }
                }
                if (ei < 0)
                {
                    return fail(anim_name + ".events 引用了不存在的事件 '" + ename + "'（参考实现报 Event not found）");
                }
                const SpineSourceEventData& data = skel.events[static_cast<size_t>(ei)];
                SpineSourceEventKey key;
                key.event = static_cast<uint32_t>(ei);
                key.int_value = GetInt(frame, "int", data.int_value);
                key.float_value = GetFloat(frame, "float", data.float_value);
                key.string_value = GetString(frame, "string", data.string_value.c_str());
                // 只有定义带音频（非空）时逐帧读，否则是 Event 构造函数的 1 / 0（不继承定义的值）
                if (!data.audio_path.empty())
                {
                    key.volume = GetFloat(frame, "volume", 1.0f);
                    key.balance = GetFloat(frame, "balance", 0.0f);
                }
                tl.frames.push_back(GetFloat(frame, "time", 0.0f));
                tl.event_keys.push_back(std::move(key));
            }
            out.timelines.push_back(std::move(tl));
        }
    }

    return Ok();
}

}   // namespace

SpineImportResult ParseSpineSkeletonJson(const char* json, size_t len, const SpineAtlas* atlas,
                                         SpineSourceSkeleton& out)
{
    out.bones.clear();
    out.slots.clear();
    out.attachments.clear();
    out.skins.clear();
    out.has_draw_order_timeline = false;
    out.uses_tint_black = false;
    out.has_clipping = false;
    out.has_event_timeline = false;
    out.animations.clear();
    out.ik_names.clear();
    out.transform_names.clear();
    out.path_names.clear();
    out.iks.clear();
    out.transforms.clear();
    out.paths.clear();
    out.path_attachments.clear();
    out.no_draw_attachments.clear();

    rapidjson::Document doc;
    const std::string text(json, len);
    if (doc.Parse(text.c_str()).HasParseError())
    {
        return Fail("json: 解析失败（offset " + std::to_string(doc.GetErrorOffset()) + "）");
    }
    if (!doc.IsObject())
    {
        return Fail("json: 根节点不是对象");
    }

    // ---- 版本：只认 3.7.x / 3.8.x ----
    // 放宽到 3.7 是因为本工程（OSGame）782 个 `.skel.bytes` **全是 3.7.83**；
    // 两者字节布局逐段比对一致，唯一差别是槽位暗色判据（见 `SpineSourceVersion` 的注释）。
    const JsonValue* skeleton = GetMember(doc, "skeleton");
    const std::string spine_ver = skeleton ? GetString(*skeleton, "spine", "") : std::string();
    {
        uint32_t runtime = 0;   // Bake 阶段由 `src.version` 重新算出，这里不用
        std::string verr;
        if (!ClassifySpineVersion(spine_ver, out.version, runtime, verr))
        {
            return Fail("json: " + verr);
        }
    }
    out.version_string = spine_ver;

    // 图集页名：容器 chunk 8（atlas_pages）要用（纹理引用）。没有图集时留空 —— 但那样需要 UV 的
    // 附件会在下面报错，不会静默产出半个资产。
    if (atlas)
    {
        out.atlas_pages.reserve(atlas->pages.size());
        for (const SpineAtlasPage& p : atlas->pages)
        {
            out.atlas_pages.push_back(p.name);
        }
    }

    // ---- bones ----
    const JsonValue* bones = GetMember(doc, "bones");
    if (bones && bones->IsArray())
    {
        for (const JsonValue& b : bones->GetArray())
        {
            SpineSourceBone bone;
            bone.name = GetString(b, "name", "");
            const std::string parent = GetString(b, "parent", "");
            bone.parent = -1;
            if (!parent.empty())
            {
                // 父骨骼必须在自己**之前** —— 参考实现也是只查已加进去的骨骼
                // （`skeletonData->findBone(parentName)`，SkeletonJson.cpp:155），
                // 所以这条不变量是格式自带的，不是我们加的约束。
                for (size_t k = 0; k < out.bones.size(); ++k)
                {
                    if (out.bones[k].name == parent)
                    {
                        bone.parent = static_cast<int>(k);
                        break;
                    }
                }
                // ⚠️ 参考实现在这里**报错**（SkeletonJson.cpp:156-160 "Parent bone not found"）。
                // 早先我们漏了这一步：查不到就把 parent 留在 -1，于是那根骨骼**静默变成
                // 根骨骼** —— 整棵子树的位移/旋转全错，而"看起来只是摆位不对"没有任何报错。
                if (bone.parent < 0)
                {
                    return Fail("json: 骨骼 '" + bone.name + "' 引用了不存在的父骨骼 '" + parent +
                                "'（参考实现在这里报 Parent bone not found）");
                }
            }

            // 默认值逐条抄 SkeletonJson.cpp:165-172。⚠️ scaleX/scaleY 的默认是 **1**。
            bone.length = GetFloat(b, "length", 0.0f);
            bone.x = GetFloat(b, "x", 0.0f);
            bone.y = GetFloat(b, "y", 0.0f);
            bone.rotation = GetFloat(b, "rotation", 0.0f);
            bone.scale_x = GetFloat(b, "scaleX", 1.0f);
            bone.scale_y = GetFloat(b, "scaleY", 1.0f);
            bone.shear_x = GetFloat(b, "shearX", 0.0f);
            bone.shear_y = GetFloat(b, "shearY", 0.0f);

            // 字段名是 **"transform"**（不是 "transformMode"）。5 个 if 都没有 else
            // （SkeletonJson.cpp:173-189）⇒ 认不出的串会被**静默留在 Normal**。
            // 那是"整棵子树的 inherit 行为变了且不报错"，所以我们显式报错。
            const std::string mode = GetString(b, "transform", "normal");
            if (mode == "normal")
            {
                bone.transform_mode = SpineTransformMode::kNormal;
            }
            else if (mode == "onlyTranslation")
            {
                bone.transform_mode = SpineTransformMode::kOnlyTranslation;
            }
            else if (mode == "noRotationOrReflection")
            {
                bone.transform_mode = SpineTransformMode::kNoRotationOrReflection;
            }
            else if (mode == "noScale")
            {
                bone.transform_mode = SpineTransformMode::kNoScale;
            }
            else if (mode == "noScaleOrReflection")
            {
                bone.transform_mode = SpineTransformMode::kNoScaleOrReflection;
            }
            else
            {
                return Fail("json: 骨骼 '" + bone.name + "' 的 transform 模式 '" + mode +
                            "' 不认识（只认 normal / onlyTranslation / noRotationOrReflection / "
                            "noScale / noScaleOrReflection —— 参考实现在这里静默退回 normal，"
                            "那会让整棵子树的继承行为悄悄变掉）");
            }

            out.bones.push_back(bone);
        }
    }

    // ---- slots ----
    const JsonValue* slots = GetMember(doc, "slots");
    if (slots && slots->IsArray())
    {
        for (const JsonValue& s : slots->GetArray())
        {
            SpineSourceSlot slot;
            slot.name = GetString(s, "name", "");
            const std::string bone_name = GetString(s, "bone", "");
            slot.bone_index = -1;
            for (size_t k = 0; k < out.bones.size(); ++k)
            {
                if (out.bones[k].name == bone_name)
                {
                    slot.bone_index = static_cast<int>(k);
                    break;
                }
            }
            if (slot.bone_index < 0)
            {
                return Fail("json: slot '" + slot.name + "' 引用了不存在的骨骼 '" + bone_name + "'");
            }
            // ---- 颜色（SkeletonJson.cpp:219-232）----
            // ⚠️ 默认值是 **(1,1,1,1)**（`SlotData.cpp:39`），不是 0。
            //    setup 色是 color / twoColor 时间轴的基线：
            //    时间轴写的是**绝对颜色**（ColorTimeline.cpp:110 `slot._color.set(r,g,b,a)`），
            //    而"没有时间轴命中"时槽位停在 setup 色 —— 容器里没有 setup 色，
            //    解算器只能默认成纯白，而**纯白与 setup 白在多数资产上完全看不出区别**
            //    （正是那种"错了但不报错"的坑）。
            if (GetMember(s, "color") != nullptr)
            {
                const std::string col = GetString(s, "color", "");
                std::string err;
                const float rgba[4] = {ToColorComponent(col, 0), ToColorComponent(col, 1),
                                       ToColorComponent(col, 2), ToColorComponent(col, 3)};
                for (int k = 0; k < 4; ++k)
                {
                    if (rgba[k] < 0.0f)
                    {
                        err = "颜色串 '" + col + "' 不是 8 位十六进制";
                        break;
                    }
                }
                if (!err.empty())
                {
                    return Fail("json: slot '" + slot.name + "'.color: " + err);
                }
                for (int k = 0; k < 4; ++k)
                {
                    slot.color[k] = rgba[k];
                }
            }
            if (GetMember(s, "dark") != nullptr)
            {
                const std::string dk = GetString(s, "dark", "");
                std::string err;
                // ⚠️ dark 只有 **rgb** 三个分量（:227-230），alpha 写死 **1**（:230）。
                const float rgb[3] = {ToColorComponent(dk, 0), ToColorComponent(dk, 1),
                                      ToColorComponent(dk, 2)};
                for (int k = 0; k < 3; ++k)
                {
                    if (rgb[k] < 0.0f)
                    {
                        err = "颜色串 '" + dk + "' 不是 6 位十六进制";
                        break;
                    }
                }
                if (!err.empty())
                {
                    return Fail("json: slot '" + slot.name + "'.dark: " + err);
                }
                for (int k = 0; k < 3; ++k)
                {
                    slot.dark[k] = rgb[k];
                }
                slot.dark[3] = 1.0f;
                slot.has_dark = true;
                out.uses_tint_black = true;
            }
            slot.attachment_name = GetString(s, "attachment", "");
            // SkeletonJson.cpp:241-250：认不出的字符串参考实现静默当 normal；这里报错。
            {
                const std::string blend = GetString(s, "blend", "normal");
                if (blend == "normal")
                {
                    slot.blend_mode = SpineBlendMode::kNormal;
                }
                else if (blend == "additive")
                {
                    slot.blend_mode = SpineBlendMode::kAdditive;
                }
                else if (blend == "multiply")
                {
                    slot.blend_mode = SpineBlendMode::kMultiply;
                }
                else if (blend == "screen")
                {
                    slot.blend_mode = SpineBlendMode::kScreen;
                }
                else
                {
                    return Fail("json: slot '" + slot.name + "' 的混合模式 '" + blend +
                                "' 不认识（normal / additive / multiply / screen）");
                }
            }
            out.slots.push_back(slot);
        }
    }

    // ---- skins ----
    // ⚠️ 3.8 的 json 是**对象**形式：skins → { 皮肤名 → { 槽位名 → { 附件名 → {...} } } }
    //    判据在参考实现自己身上：SkeletonJson.cpp:439 `new Skin(skinMap->_name)` ——
    //    皮肤名取自**成员名**；而 Json.cpp:402-450 的 parseArray 里数组成员是
    //    `new Json(NULL)`、之后**从不**赋 _name —— 数组形式会拿到空名。
    //    数组形式（[{ "name": ..., "attachments": {...} }]）是后面的版本才有的，
    //    3.8 的参考实现读不了；这里顺手接住，皮肤名改取 "name" 字段。
    struct SkinEntry
    {
        std::string name;
        const JsonValue* atts = nullptr;
    };
    std::vector<SkinEntry> skin_entries;
    const JsonValue* skins = GetMember(doc, "skins");
    if (skins && skins->IsArray())
    {
        for (const JsonValue& sk : skins->GetArray())
        {
            if (const JsonValue* atts = GetMember(sk, "attachments"))
            {
                skin_entries.push_back(SkinEntry {GetString(sk, "name", ""), atts});
            }
        }
    }
    else if (skins && skins->IsObject())
    {
        for (auto it = skins->MemberBegin(); it != skins->MemberEnd(); ++it)
        {
            skin_entries.push_back(SkinEntry {std::string(it->name.GetString()), &it->value});
        }
    }

    // 皮肤名必须唯一：重名会让"按名换装"落到哪一个变不确定
    // （参考实现 findSkin() 取第一个，是**静默**的）。
    for (const SkinEntry& se : skin_entries)
    {
        for (const std::string& s : out.skins)
        {
            if (s == se.name)
            {
                return Fail("json: 重复的皮肤名 '" + se.name + "'（换装会按名字查，重名取哪个是不确定的）");
            }
        }
        out.skins.push_back(se.name);
    }

    // 收集：**不再跨皮肤合并** —— 同一个 (槽位, 附件名) 在不同皮肤下是两份几何。
    struct PendingAttachment
    {
        int skin_index;
        int slot_index;
        std::string name;
        const JsonValue* json;
    };
    std::vector<PendingAttachment> pending;

    for (size_t si = 0; si < skin_entries.size(); ++si)
    {
        const JsonValue* atts = skin_entries[si].atts;
        if (!atts || !atts->IsObject())
        {
            continue;
        }
        for (auto slot_it = atts->MemberBegin(); slot_it != atts->MemberEnd(); ++slot_it)
        {
            const std::string slot_name(slot_it->name.GetString());
            int slot_index = -1;
            for (size_t k = 0; k < out.slots.size(); ++k)
            {
                if (out.slots[k].name == slot_name)
                {
                    slot_index = static_cast<int>(k);
                    break;
                }
            }
            if (slot_index < 0 || !slot_it->value.IsObject())
            {
                continue;
            }
            for (auto att_it = slot_it->value.MemberBegin(); att_it != slot_it->value.MemberEnd(); ++att_it)
            {
                const std::string att_name(att_it->name.GetString());
                bool dup = false;
                for (const PendingAttachment& p : pending)
                {
                    if (p.skin_index == static_cast<int>(si) && p.slot_index == slot_index && p.name == att_name)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                {
                    pending.push_back(PendingAttachment {static_cast<int>(si), slot_index, att_name,
                                                         &att_it->value});
                }
            }
        }
    }

    // 按 (皮肤, 绘制顺序) 排：先 default 皮肤的全部附件，再下一个皮肤。
    std::sort(pending.begin(), pending.end(),
              [](const PendingAttachment& a, const PendingAttachment& b)
              {
                  if (a.skin_index != b.skin_index)
                  {
                      return a.skin_index < b.skin_index;
                  }
                  return a.slot_index < b.slot_index;
              });

    // linked mesh 要等所有附件都解析完才能解引用父附件（父可能在**后面**的 skin 里）
    std::vector<SpineLinkedMeshRef> linked;

    for (const PendingAttachment& p : pending)
    {
        const JsonValue& a = *p.json;
        const std::string type = GetString(a, "type", "region");

        // json 里 linked mesh 有两种写法：type="linkedmesh"，或 type="mesh" 带 "parent"
        // （参考实现 SkeletonJson.cpp:462-465 判类型、:538 判 parent）。
        const JsonValue* jparent = GetMember(a, "parent");
        if (type == "linkedmesh" || (type == "mesh" && jparent != nullptr))
        {
            SpineLinkedMeshRef lm;
            lm.slot_index = p.slot_index;
            lm.skin_index = p.skin_index;
            lm.name = p.name;
            lm.path = GetString(a, "path", "");
            lm.parent = jparent != nullptr && jparent->IsString() ? jparent->GetString() : std::string();
            lm.skin = GetString(a, "skin", "");
            if (lm.parent.empty())
            {
                return Fail("json: 附件 '" + lm.name + "' 标成 linked mesh 但没有 parent");
            }
            // json 里这个字段就叫 **"deform"**（不是 "inheritDeform"），默认 1
            // （SkeletonJson.cpp:572 `Json::getInt(attachmentMap, "deform", 1)`）。
            // 它决定父附件的 deform 是否也作用于继承方
            // （MeshAttachment::applyDeform，:94-95）⇒ 导入期要用：继承方得一起回退。
            lm.inherit_deform = GetInt(a, "deform", 1) != 0;
            linked.push_back(std::move(lm));
            continue;
        }

        if (type == "clipping")
        {
            // 字段顺序照抄参考实现（SkeletonJson.cpp:625-640）："end" 槽位名 →
            // vertexCount → vertices。
            //
            // ⚠️ `Json::getString(map, "end", 0)` 拿到名字后 `findSlot()` 的返回值
            // **没有检查**（:631-635）⇒ 槽位名写错 / 槽位不存在时 `_endSlot` 静默变 NULL，
            // 作用域从"裁到某个槽"变成"裁到最后一个槽"。画面差别极大却没有任何报错
            // （二进制那边反而是显式报错的，见下面 ParseSkelAttachment）⇒ 这里必须报错。
            SpineSourceClip clip;
            clip.skin_index = p.skin_index;
            clip.slot_index = p.slot_index;
            clip.name = p.name;
            const std::string end_name = GetString(a, "end", "");
            if (!end_name.empty())
            {
                const int end_index = FindSlotIndex(out.slots, end_name);
                if (end_index < 0)
                {
                    return Fail("json: 裁剪附件 '" + p.name + "'（槽位 '" +
                                out.slots[static_cast<size_t>(p.slot_index)].name + "'）的 end 槽位 '" +
                                end_name + "' 不存在（参考实现在这里静默当成 NULL ⇒ 裁剪一路延伸到最后一个槽位）");
                }
                clip.end_slot_index = end_index;
            }
            // 没写 "end"（或写了空串）⇒ end_slot_index 保持 -1 ⇒ 裁到最后一个槽位。

            // ---- 裁剪多边形本身（SkeletonJson.cpp:636-637）----
            // `vertexCount = Json::getInt(map, "vertexCount", 0)` 是**顶点数**，
            // readVertices 里拿它当 worldVerticesLength（float 数 = 顶点数 × 2）。
            const int vertex_count = GetInt(a, "vertexCount", 0);
            const JsonValue* jverts = GetMember(a, "vertices");
            // ⚠️ 参考实现在这里有个坑：缺 "vertexCount" ⇒ verticesLength == 0 ≠ entrySize
            // ⇒ `readVertices` 会走 **weighted** 分支（:1218 的判据），把一串局部坐标
            // 当成 [n, (bone,x,y,w)×n] 去解析 ⇒ 多边形变成一堆垃圾骨骼索引，且不报错。
            // 这里必须显式挡住。
            if (vertex_count <= 0 || jverts == nullptr || !jverts->IsArray())
            {
                return Fail("json: 裁剪附件 '" + p.name + "'（槽位 '" +
                            out.slots[static_cast<size_t>(p.slot_index)].name +
                            "'）缺 vertexCount 或 vertices（参考实现在这里会把 rigid 多边形"
                            "误判成 weighted ⇒ 静默解析成垃圾骨骼索引）");
            }
            clip.vertex_count = vertex_count;
            clip.vertices.reserve(jverts->Size());
            for (const JsonValue& f : jverts->GetArray())
            {
                clip.vertices.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }
            // ⚠️ weighted 的判据照抄 `readVertices`（:1218）：**数组长度 == vertexCount*2
            // 才是 rigid**。裁剪附件不贴图 ⇒ 没有 uvs 可比对，只能比这个数。
            clip.weighted = clip.vertices.size() != static_cast<size_t>(vertex_count) * 2u;

            // deform 可以指向它（ClippingAttachment 也是 VertexAttachment）⇒ 与二进制侧
            // （RecordNoDrawAttachment）对齐进"不渲染附件表"，否则指向它的 deform 时间轴
            // 会被当成"附件不存在"报错（v14 及以前 json 路径就是这样，二进制路径没有）。
            SpineSourceNoDrawAttachment nd;
            nd.skin_index = p.skin_index;
            nd.slot_index = p.slot_index;
            nd.name = p.name;
            out.no_draw_attachments.push_back(std::move(nd));

            out.clips.push_back(clip);
            out.has_clipping = true;
            continue;
        }

        SpineSourceAttachment att;
        att.name = p.name;
        att.path = GetString(a, "path", "");
        att.slot_index = p.slot_index;
        att.skin_index = p.skin_index;

        const std::string region_name = att.path.empty() ? att.name : att.path;
        const SpineAtlasRegion* reg = atlas ? FindAtlasRegion(*atlas, region_name) : nullptr;
        if (reg != nullptr)
        {
            att.atlas_page = reg->page;
        }

        if (type == "region")
        {
            att.type = SpineSourceAttachment::Type::kRegion;

            const float x = GetFloat(a, "x", 0.0f);
            const float y = GetFloat(a, "y", 0.0f);
            const float w = GetFloat(a, "width", 0.0f);
            const float h = GetFloat(a, "height", 0.0f);
            const float sx = GetFloat(a, "scaleX", 1.0f);
            const float sy = GetFloat(a, "scaleY", 1.0f);
            const float rot = GetFloat(a, "rotation", 0.0f);

            if (!reg)
            {
                return Fail("atlas: 附件 '" + att.name + "'（槽位 '" + out.slots[static_cast<size_t>(p.slot_index)].name +
                            "'）找不到图集 region '" + region_name + "'。缺图集就算不出 UV —— 不拿零 UV 糊过去。");
            }

            // 未裁剪：region 尺寸即原始尺寸，偏移为 0
            float xy[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            ComputeRegionVertices(x, y, w, h, sx, sy, rot, static_cast<float>(reg->width),
                                  static_cast<float>(reg->height), static_cast<float>(reg->offset_x),
                                  static_cast<float>(reg->offset_y), static_cast<float>(reg->orig_w),
                                  static_cast<float>(reg->orig_h), xy);
            att.vertices.assign(xy, xy + 8);

            float uv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            ComputeRegionUvs(*reg, uv);
            att.uvs.assign(uv, uv + 8);

            // Spine 的 region 三角形固定是 BL,UL,UR / UR,BR,BL
            att.triangles = {0, 1, 2, 2, 3, 0};
        }
        else if (type == "mesh")
        {
            att.type = SpineSourceAttachment::Type::kMesh;

            const JsonValue* juvs = GetMember(a, "uvs");
            const JsonValue* jtri = GetMember(a, "triangles");
            const JsonValue* jverts = GetMember(a, "vertices");
            if (!juvs || !juvs->IsArray() || !jtri || !jtri->IsArray() || !jverts || !jverts->IsArray())
            {
                return Fail("json: mesh 附件 '" + att.name + "' 缺少 uvs / triangles / vertices");
            }

            std::vector<float> region_uvs;
            region_uvs.reserve(juvs->Size());
            for (const JsonValue& f : juvs->GetArray())
            {
                region_uvs.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }

            att.triangles.reserve(jtri->Size());
            for (const JsonValue& t : jtri->GetArray())
            {
                att.triangles.push_back(static_cast<uint16_t>(t.GetUint()));
            }

            att.vertices.reserve(jverts->Size());
            for (const JsonValue& f : jverts->GetArray())
            {
                att.vertices.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }

            if (!reg)
            {
                return Fail("atlas: mesh 附件 '" + att.name + "'（槽位 '" +
                            out.slots[static_cast<size_t>(p.slot_index)].name + "'）找不到图集 region '" + region_name +
                            "'。json 里的 uvs 是图集空间坐标，没有图集变不成最终 UV。");
            }

            std::vector<float> final_uvs;
            ComputeMeshUvs(*reg, region_uvs, final_uvs);
            att.uvs = std::move(final_uvs);
            // 留给将来引用它的 linked mesh（它要拿父的 regionUVs 配自己的 region 重算）
            att.region_uvs = std::move(region_uvs);
        }
        else if (type == "path")
        {
            // PathAttachment（chunk 20，v11）。**不产生绘制项**，但 path 约束要沿它
            // 摆骨骼 ⇒ 必须烘焙几何。字段抄 SkeletonJson.cpp:588-604。
            //
            // ⚠️ `vertexCount` 是**点数**不是 float 数：参考实现把它 `<< 1` 之后
            //    才交给 readVertices（:598），而 lengths 用的是**没移位**的那个
            //    （:600 `vertexCount / 3`）。与二进制侧一致（SkeletonBinary.cpp:548-550
            //    直接把 varint 当点数传给 readVertices，lengths = 点数 / 3）。
            SpineSourcePathAttachment pa;
            pa.skin_index = p.skin_index;
            pa.slot_index = p.slot_index;
            pa.name = p.name;
            pa.closed = GetInt(a, "closed", 0) != 0;
            // ⚠️ 默认 **1**（:596）—— 与 offset 类字段的默认 0 不一样，记反了
            //    表现是"沿路径匀速 vs 按曲线参数匀速"，肉眼看不出来。
            pa.constant_speed = GetInt(a, "constantSpeed", 1) != 0;
            pa.vertex_count = GetInt(a, "vertexCount", 0);
            if (pa.vertex_count < 0)
            {
                return Fail("json: path 附件 '" + pa.name + "' 的 vertexCount 是负的");
            }
            const JsonValue* jverts = GetMember(a, "vertices");
            if (jverts == nullptr || !jverts->IsArray())
            {
                return Fail("json: path 附件 '" + pa.name + "' 缺 vertices");
            }
            std::vector<float> flat;
            flat.reserve(jverts->Size());
            for (const JsonValue& f : jverts->GetArray())
            {
                flat.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
            }
            // weighted 的判据照抄 json `readVertices`（:1208 `verticesLength == entrySize`）：
            // 顶点数组长度 == 点数 × 2 才是 rigid。
            const bool weighted = flat.size() != static_cast<size_t>(pa.vertex_count) * 2u;
            pa.weighted = weighted;
            std::string verr;
            if (!SplitPathVertices(flat, pa.vertex_count, weighted, pa.bones, pa.vertices, verr))
            {
                return Fail("json: path 附件 '" + pa.name + "': " + verr);
            }
            if (const JsonValue* jlen = GetMember(a, "lengths"); jlen != nullptr && jlen->IsArray())
            {
                pa.lengths.reserve(jlen->Size());
                for (const JsonValue& f : jlen->GetArray())
                {
                    pa.lengths.push_back(f.IsNumber() ? f.GetFloat() : 0.0f);
                }
            }
            // 参考实现永远写 vertexCount/3 条（setSize，多的截断、少的补 0），
            // 我们按声明的值校验 —— 少了说明对不上，多了是资产自己带的脏数据。
            if (pa.lengths.size() < static_cast<size_t>(pa.vertex_count) / 3u)
            {
                return Fail("json: path 附件 '" + pa.name + "' 的 lengths 只有 " +
                            std::to_string(pa.lengths.size()) + " 条，声明需要 " +
                            std::to_string(pa.vertex_count / 3) + " 条");
            }
            pa.lengths.resize(static_cast<size_t>(pa.vertex_count) / 3u);

            // deform 可以指向它（PathAttachment 也是 VertexAttachment）⇒ 也要进
            // "不渲染附件表"，否则 Bake 会把指向它的 deform 当成找不到附件而报错。
            SpineSourceNoDrawAttachment nd;
            nd.skin_index = p.skin_index;
            nd.slot_index = p.slot_index;
            nd.name = p.name;
            out.no_draw_attachments.push_back(std::move(nd));

            out.path_attachments.push_back(std::move(pa));
            continue;   // 不进 out.attachments —— 它不产生绘制项
        }
        else if (type == "boundingbox" || type == "point")
        {
            // 与二进制侧对齐（RecordNoDrawAttachment）：不烘焙，但 deform 指向它们
            // 是"没有顶点可改"而不是错误 ⇒ 记进不渲染附件表。
            SpineSourceNoDrawAttachment nd;
            nd.skin_index = p.skin_index;
            nd.slot_index = p.slot_index;
            nd.name = p.name;
            out.no_draw_attachments.push_back(std::move(nd));
            continue;
        }
        else
        {
            return Fail("json: 附件 '" + att.name + "' 的类型 '" + type + "' 不支持");
        }

        out.attachments.push_back(std::move(att));
    }

    // ---- 约束 setup（chunk 18，v11）----
    // 字段逐条抄 3.7 `SkeletonJson.cpp:255-422`；名字表与 setup **必须同序**
    // （动画里的时间轴按名字查下标，下标 = 它在数组里的位置）。
    //
    // 三处默认值要盯住，它们都是"记错就静默错姿势"的：
    //   1. `bendPositive` 是**布尔**，取反成 -1（:291）—— 不是 `bendDirection`。
    //   2. `mix` / 四个 `*:Mix` 默认 **1**（:290、:344-347、:418-419）。
    //   3. path 的三个 mode 串在**缺 key** 与**认不出的串**两种情况下落到的值
    //      **不一样**：缺 key 时 getString 的 fallback 是 "percent"/"length"/"tangent"
    //      ⇒ Percent(1)/Length(0)/Tangent(0)；认不出的串则三个 if 都不进，
    //      留在 PathConstraintData 构造里的默认 Fixed(0)/Length(0)/Tangent(0)
    //      （PathConstraintData.cpp:48-50）⇒ position 变成 Fixed。
    //      这是参考实现自己的不一致 ⇒ 我们不猜，**显式报错**。
    //
    // ⚠️ `_scale`：参考实现把 offsetX/offsetY 和 spacing 乘 `_scale`
    //    （:332-333、:410-417）。本解析器全程按 scale == 1 工作（bone 的
    //    length / x / y 也没乘，见上面 bones 段），所以这里同样不乘。
    {
        // 骨骼名数组 → 下标数组。参考实现查不到时**报错**（IK bone not found 等）。
        std::string bone_err;
        const auto read_bones = [&](const JsonValue& c, const char* what, const std::string& cname,
                                    std::vector<int>& dst) -> bool {
            const JsonValue* jb = GetMember(c, "bones");
            if (jb == nullptr || !jb->IsArray())
            {
                bone_err = "json: " + std::string(what) + " 约束 '" + cname + "' 缺 bones 数组";
                return false;
            }
            for (const JsonValue& bn : jb->GetArray())
            {
                const std::string bn_name = bn.IsString() ? std::string(bn.GetString()) : std::string();
                const int bi = FindBoneIndex(out.bones, bn_name);
                if (bi < 0)
                {
                    bone_err = "json: " + std::string(what) + " 约束 '" + cname + "' 引用了不存在的骨骼 '" +
                               bn_name + "'（参考实现在这里报 " + what + " bone not found）";
                    return false;
                }
                dst.push_back(bi);
            }
            if (dst.empty())
            {
                // 参考实现允许 0 条骨骼（循环不进），但那样的约束什么也不做 ——
                // 而"什么也不做"和"漏解析"在结果上无法区分 ⇒ 报错。
                bone_err = "json: " + std::string(what) + " 约束 '" + cname + "' 的 bones 是空的";
                return false;
            }
            return true;
        };

        if (const JsonValue* arr = GetMember(doc, "ik"); arr != nullptr && arr->IsArray())
        {
            for (const JsonValue& c : arr->GetArray())
            {
                SpineSourceIkConstraint k;
                k.name = GetString(c, "name", "");
                k.order = GetInt(c, "order", 0);
                if (!read_bones(c, "IK", k.name, k.bones))
                {
                    return Fail(bone_err);
                }
                const std::string tname = GetString(c, "target", "");
                k.target = FindBoneIndex(out.bones, tname);
                if (k.target < 0)
                {
                    return Fail("json: IK 约束 '" + k.name + "' 的目标骨骼 '" + tname +
                                "' 不存在（参考实现在这里报 Target bone not found）");
                }
                k.mix = GetFloat(c, "mix", 1.0f);
                k.bend_dir = GetInt(c, "bendPositive", 1) ? 1 : -1;
                k.compress = GetInt(c, "compress", 0) != 0;
                k.stretch = GetInt(c, "stretch", 0) != 0;
                k.uniform = GetInt(c, "uniform", 0) != 0;
                out.ik_names.push_back(k.name);
                out.iks.push_back(std::move(k));
            }
        }

        if (const JsonValue* arr = GetMember(doc, "transform"); arr != nullptr && arr->IsArray())
        {
            for (const JsonValue& c : arr->GetArray())
            {
                SpineSourceTransformConstraint t;
                t.name = GetString(c, "name", "");
                t.order = GetInt(c, "order", 0);
                if (!read_bones(c, "transform", t.name, t.bones))
                {
                    return Fail(bone_err);
                }
                const std::string tname = GetString(c, "target", "");
                t.target = FindBoneIndex(out.bones, tname);
                if (t.target < 0)
                {
                    return Fail("json: transform 约束 '" + t.name + "' 的目标骨骼 '" + tname +
                                "' 不存在（参考实现在这里报 Target bone not found）");
                }
                t.local = GetInt(c, "local", 0) != 0;
                t.relative = GetInt(c, "relative", 0) != 0;
                t.offset_rotation = GetFloat(c, "rotation", 0.0f);
                t.offset_x = GetFloat(c, "x", 0.0f);
                t.offset_y = GetFloat(c, "y", 0.0f);
                t.offset_scale_x = GetFloat(c, "scaleX", 0.0f);
                t.offset_scale_y = GetFloat(c, "scaleY", 0.0f);
                t.offset_shear_y = GetFloat(c, "shearY", 0.0f);
                t.rotate_mix = GetFloat(c, "rotateMix", 1.0f);
                t.translate_mix = GetFloat(c, "translateMix", 1.0f);
                t.scale_mix = GetFloat(c, "scaleMix", 1.0f);
                t.shear_mix = GetFloat(c, "shearMix", 1.0f);
                out.transform_names.push_back(t.name);
                out.transforms.push_back(std::move(t));
            }
        }

        if (const JsonValue* arr = GetMember(doc, "path"); arr != nullptr && arr->IsArray())
        {
            for (const JsonValue& c : arr->GetArray())
            {
                SpineSourcePathConstraint p;
                p.name = GetString(c, "name", "");
                p.order = GetInt(c, "order", 0);
                if (!read_bones(c, "path", p.name, p.bones))
                {
                    return Fail(bone_err);
                }
                // ⚠️ path 的 target 是**槽位**（findSlot，SkeletonJson.cpp:376）
                const std::string tname = GetString(c, "target", "");
                p.target_slot = FindSlotIndex(out.slots, tname);
                if (p.target_slot < 0)
                {
                    return Fail("json: path 约束 '" + p.name + "' 的目标**槽位** '" + tname +
                                "' 不存在（参考实现在这里报 Target slot not found）");
                }
                const std::string pm = GetString(c, "positionMode", "percent");
                if (pm == "fixed")
                {
                    p.position_mode = 0;
                }
                else if (pm == "percent")
                {
                    p.position_mode = 1;
                }
                else
                {
                    return Fail("json: path 约束 '" + p.name + "' 的 positionMode '" + pm +
                                "' 认不出（只认 fixed / percent）");
                }
                const std::string sm = GetString(c, "spacingMode", "length");
                if (sm == "length")
                {
                    p.spacing_mode = 0;
                }
                else if (sm == "fixed")
                {
                    p.spacing_mode = 1;
                }
                else if (sm == "percent")
                {
                    p.spacing_mode = 2;
                }
                else
                {
                    return Fail("json: path 约束 '" + p.name + "' 的 spacingMode '" + sm +
                                "' 认不出（只认 length / fixed / percent）");
                }
                const std::string rm = GetString(c, "rotateMode", "tangent");
                if (rm == "tangent")
                {
                    p.rotate_mode = 0;
                }
                else if (rm == "chain")
                {
                    p.rotate_mode = 1;
                }
                else if (rm == "chainScale")
                {
                    p.rotate_mode = 2;
                }
                else
                {
                    return Fail("json: path 约束 '" + p.name + "' 的 rotateMode '" + rm +
                                "' 认不出（只认 tangent / chain / chainScale）");
                }
                p.offset_rotation = GetFloat(c, "rotation", 0.0f);
                p.position = GetFloat(c, "position", 0.0f);
                p.spacing = GetFloat(c, "spacing", 0.0f);
                p.rotate_mix = GetFloat(c, "rotateMix", 1.0f);
                p.translate_mix = GetFloat(c, "translateMix", 1.0f);
                out.path_names.push_back(p.name);
                out.paths.push_back(std::move(p));
            }
        }
    }

    // ---- events（v16，SkeletonJson.cpp:673-694）：必须在 animations 之前（时间轴按名字找定义）----
    out.events.clear();
    if (const JsonValue* events = GetMember(doc, "events"))
    {
        if (!events->IsObject())
        {
            return Fail("json: events 不是对象");
        }
        for (auto it = events->MemberBegin(); it != events->MemberEnd(); ++it)
        {
            SpineSourceEventData e;
            e.name = it->name.GetString();
            if (e.name.empty())
            {
                return Fail("json: events 里有空名字的事件");
            }
            const JsonValue& v = it->value;
            e.int_value = GetInt(v, "int", 0);
            e.float_value = GetFloat(v, "float", 0.0f);
            e.string_value = GetString(v, "string", "");
            e.audio_path = GetString(v, "audio", "");
            // ⚠️ 参考实现是 `if (audioPath)` —— 指针非空。json 里写了 `"audio": ""` 时指针非空、
            //    串是空的，于是 volume / balance 照读；但事件派发时 `getAudioPath().isEmpty()` 为真。
            //    这里按"键存在"读，与它一致。
            if (GetMember(v, "audio") != nullptr)
            {
                e.volume = GetFloat(v, "volume", 1.0f);
                e.balance = GetFloat(v, "balance", 0.0f);
            }
            out.events.push_back(std::move(e));
        }
    }

    // ---- animations：整段烘焙（v9 chunk 11-16）----
    // deform **仍然**要单独收一份引用：它要落到附件上把绘制项标成 kFallback，
    // 而这件事必须等 linked mesh 解引用完才能做（deform 可以指向 linked mesh）。
    std::vector<SpineDeformRef> deform_refs;
    const JsonValue* anims = GetMember(doc, "animations");
    if (anims && anims->IsObject())
    {
        for (auto it = anims->MemberBegin(); it != anims->MemberEnd(); ++it)
        {
            const JsonValue& anim = it->value;
            const std::string anim_name(it->name.GetString());
            if (!anim.IsObject())
            {
                continue;
            }
            if (GetMember(anim, "drawOrder") || GetMember(anim, "draworder"))
            {
                out.has_draw_order_timeline = true;
            }
            if (const JsonValue* ev = GetMember(anim, "events"))
            {
                if (ev->IsArray() && ev->Size() > 0)
                {
                    out.has_event_timeline = true;   // 时间轴本身在 ParseSpineAnimationJson 里解析（v16）
                }
            }

            SpineSourceAnimation parsed;
            parsed.name = anim_name;
            const SpineImportResult r = ParseSpineAnimationJson(anim, anim_name, out, parsed);
            if (!r.ok)
            {
                return r;
            }
            for (const SpineSourceTimeline& tl : parsed.timelines)
            {
                if (tl.type != SpineTimelineType::kDeform)
                {
                    continue;
                }
                SpineDeformRef ref;
                ref.skin_index = tl.deform_skin;
                ref.slot_index = tl.deform_slot;
                ref.attachment = tl.deform_attachment;
                ref.skin_name = out.skins[static_cast<size_t>(tl.deform_skin)];
                ref.slot_name = out.slots[static_cast<size_t>(tl.deform_slot)].name;
                for (const SpineSourceDeformFrame& df : tl.deform_frames)
                {
                    const size_t need = static_cast<size_t>(df.start) + df.values.size();
                    if (need > ref.required_length)
                    {
                        ref.required_length = need;
                    }
                }
                deform_refs.push_back(std::move(ref));
            }
            out.animations.push_back(std::move(parsed));
        }
    }

    // ---- linked mesh：所有皮肤读完后才解引用父附件 ----
    const bool need_sort = !linked.empty();
    if (need_sort)
    {
        std::string err;
        if (!ResolveLinkedMeshes(atlas, out.skins, out.slots, out.attachments, linked, err))
        {
            return Fail("json: " + err);
        }
    }

    // ---- deform：linked mesh 解完之后才落，且必须在重排之前（传播要用 parent_index）----
    if (!deform_refs.empty())
    {
        std::string err;
        if (!ApplyDeformRefs(out.attachments, deform_refs, out.no_draw_attachments, err))
        {
            return Fail("json: " + err);
        }
    }

    if (need_sort)
    {
        // 解出来的附件插在末尾 ⇒ 重排一次，保证 (皮肤, 槽位) 有序。
        StableSortAttachmentsKeepParents(out.attachments,
                         [](const SpineSourceAttachment& a, const SpineSourceAttachment& b)
                         {
                             if (a.skin_index != b.skin_index)
                             {
                                 return a.skin_index < b.skin_index;
                             }
                             return a.slot_index < b.slot_index;
                         });
    }

    return Ok();
}
