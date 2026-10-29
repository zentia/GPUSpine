# P2 / P3 修改方案与测试用例（deform 剩下的两块）

## 0. 这份文档管什么

承接 `doc/PERF_ACCEPTANCE.md` §1.2：anim 段比参考慢，根源**只有 deform 一件事**（两侧同时消融
实测占差距 **100.2%**）；`__restrict` 那条线已经扫完并到头（第二批只值 ~0.3%）。

> ⚠️ **2026-10-29 修订（`PERF_ACCEPTANCE.md` §1.4）**：改用**逐帧交错**重测后，
> - anim 的差距里 **非 deform 占 60%**、deform 干活占 24%、**解析 + 早退只占 17%（102.5 µs = 我们 anim 的 3%）**；
> - 也就是说 **P2 的收益上限比本文档写作时认为的小一个量级**，本文档的绝对值（214.8 / 391.9 µs）
>   是"改前改后各跑一遍"的消融估计值，偏大 ~2 倍（见 `PERF_ACCEPTANCE.md` §1.3 第 4 条）。
> - **动 P2/P3 之前先按 §1.4 的方法重测一遍**，别按本文档的旧数字排优先级。

deform 内部拆开之后，剩下**两块没动过**：

| 块 | Δ µs | 占 deform | 方案 |
|---|---|---|---|
| **解析 + 早退**（只解析、不干活） | **214.8** | 17.7% | **P2**（本文档 §2） |
| "其余"（末帧 / 早帧 / `SetFixedSize`） | **391.9** | 32.3% | **P3**（本文档 §3） |
| （稀疏展开 239.8 / 混合循环 365.6） | 605.4 | 50.0% | 已确认是**格式取舍**（只存增量）的固有成本，见 §1.2 注解 |

⚠️ 中间那 605.4 µs 不是"没做"，是**已经试过**：P1 融合快路径做完了、访存降 60%、
**实测反而慢 1.7~3.5%**（SIMD 受限不是带宽受限）。要动它只剩 P4（载入期稠密化），
实测 **96 MB**，不划算。

**每个方案都给**：现状代码 → 改法（含数据结构）→ 收益预估 → 风险 → 测试用例 → A/B 方法。

---

## 1. 共同前提

### 1.1 门禁（每次改动都要过，命令见 `PERF_ACCEPTANCE.md` §6）

1. `ctest --test-dir build -C Debug` **全绿**（Import / HotLoop / CSharpParity）
2. 781 资产对拍 `=== verdict: PASS ===`、`分叉的资产 : 0`
3. `SpineGpuHotLoopTest`：**稳态每帧 0 次堆分配**
4. Release 基准比值不退：`anim ≥ 0.80x`、`total ≥ 1.43x`、`geo ≥ 2.3x`

### 1.2 A/B 规矩（照抄 `PERF_ACCEPTANCE.md` §1.3，不看必错）

- 绝对值漂 50%（两边一起漂）⇒ 只能看比值；但**判收益要看我们侧的绝对耗时**。
- **低于 ~2% 只能靠"同一份二进制 + env 开关 + 交错跑"** ⇒ 每个改动都必须带 env 开关，
  测完开关保留还是删除视收益大小定（有收益的保留成"可回退的开关"，无收益的连同代码删掉）。
- 消融（跳过一段代码）估的值偏大 ~2 倍 ⇒ 只用来找"有没有"，定收益要交错跑。

### 1.3 新增测试怎么挂

`Tests~/CMakeLists.txt` 已经封好了：`gpuspine_add_test(<名字> <cpp>)` 会自动链
`GPUSpineTestable`（核心副本 + json 夹具解析器）并挂好 include；**要进 ctest 再补一行
`add_test(NAME ... COMMAND ...)`**。

⚠️ 数分配的目标**必须单独一份**（不能链别人的 `operator new`），照 `SpineGpuHotLoopTest`
的写法（独立 `add_executable` + `_ITERATOR_DEBUG_LEVEL=0`）。
⚠️ 需要传参数（资产目录）的**不要**进 ctest，手动跑。

---

## 2. P2：解析 + 早退（214.8 µs，17.7%）

### 2.1 现状

`ApplyDeform` 开头那一段（`Runtime/GPUSpine/SpineGpuSkeletonState.cpp`）每条 deform 时间轴
**每帧**都要走一遍：

1. 解 `tl.target` 的位域（`kDeformTargetPathBit` / `kDeformTargetClipBit`）
2. 越界校验（`path_attachments` / `clip_polygons` / `attachment_records`）
3. 取 `asset.path_attachments[k]` / `asset.clip_polygons[k]` / `asset.attachment_records[tl.target]` → `slot_index`
4. 读 `st.slots[slot_index].attachment`，判"槽位此刻挂的是不是我"：
   - path：`attachment.path_att == k`
   - clip：`attachment.clip == k`
   - mesh：`draw_item` → `asset.draw_items[item]` →
     `di.attachment_id == rec.attachment_id || di.deform_parent == rec.attachment_id`
5. 通过了才取 `deform_len` / `rigid` / `setup`

第 3、4 步是**依赖加载**（指针追到不同的数组里），而且是**三路分支**。

**为什么值得做**：分支画像实测进来 15,240,246 条次，只有 **40.4%** 真干活 ——
**59.6% 只付了"解析 + 早退"这笔钱就走了**。

### 2.2 改法

#### P2a：预解析（加载期，进 `SpineGpuRuntimeData`）

把第 1~3 步前移，运行时变成**按 `tl_index` 一次连续读**：

```cpp
enum class DeformKind : uint8_t { kMesh = 0, kPath = 1, kClip = 2 };

struct DeformTarget
{
    uint32_t   slot_index;   // 槽位
    DeformKind kind;
    uint32_t   key;          // mesh: attachment_id；path/clip: chunk 里的下标
    uint32_t   key_alt;      // mesh: deform_parent（inheritDeform 的父 id）；其余 = kNone
};

// SpineGpuRuntimeData 新增：
std::vector<DeformTarget> deform_target;   // [timeline]，非 deform 时间轴留空（不读）
```

#### P2b：解析结果缓存 + per-slot 版本号（每实例）

`SpineGpuSlotState` 加 `uint32_t att_version`（**任何**改写 `attachment` 的地方都要 ++），
再给每个实例一份解析缓存（实例创建时按时间轴数**一次**分配）：

```cpp
struct DeformResolve
{
    uint32_t       slot_ver = kInvalidVersion;
    uint32_t       item;         // kNone = 不产生绘制项
    uint32_t       deform_len;
    bool           rigid;
    const float*   setup;        // rigid 的 setup 顶点；weighted = nullptr
    bool           applies;      // 槽位此刻挂的是不是我
};

// SpineGpuSkeletonState 新增：
std::vector<DeformResolve> deform_resolve;   // [timeline]
```

运行时：

```cpp
const DeformTarget& dt = data.deform_target[tl_index];
const uint32_t      ver = st.slots[dt.slot_index].att_version;
DeformResolve&      cr = st.deform_resolve[tl_index];
if (cr.slot_ver != ver)
{
    /* 完整解析一遍（= 现在的 1~5 步），写进 cr */
    cr.slot_ver = ver;
}
if (!cr.applies)
{
    return Ok();          // 59.6% 在这里走掉，成本 = 1 次 u32 比较
}
```

⇒ 解析从"每帧每条时间轴 O(依赖加载)"降到 **1 次 u32 比较 + 1 次连续读**。

### 2.3 收益预估

**−80~150 µs**（deform 的 17.7%）。按现在的账：我们 anim 合计 ~3308 µs / 参考 2645 µs
（差 663 µs），砍掉 130 µs ⇒ 差 533 µs、**anim 比值 0.80x → ~0.83x**。

### 2.4 ⚠️ 风险

| 风险 | 说明 |
|---|---|
| **`deform_len` 不能预解析** | mesh 的 `deform_len = data.item_deform_len[item]`，依赖**当前** draw_item，只有身份校验通过后才确定 ⇒ 只能进**缓存**，不能进预解析表 |
| **版本号要盖住全部改写点** | attachment 时间轴、`setSkin`、骨架复位、draw_order 变化（draw_item 会跟着变）。**漏一个 ⇒ deform 静默写错槽位，只有对拍会红** |
| **换肤的语义** | 换肤后同名附件来自另一个皮肤 = **另一个对象** ⇒ deform 要清（参考实现就是这个行为）。版本号必须按 `SpineGpuAttachRef` 的**身份**（`name_id` + `skin`）走，不能只按名字 |
| **0 分配** | `deform_resolve` 只能在实例创建时分配一次；每帧改它的 size 会让 `SpineGpuHotLoopTest` 红 |
| 内存 | 每实例 ~（deform 时间轴数）× 24 B。OSGame 781 资产平均 ~128 条/资产 ⇒ ~3 KB/实例，可接受 |

### 2.5 测试用例

| 编号 | 归属 | 输入 / 夹具 | 断言 | 为什么值得钉 |
|---|---|---|---|---|
| **T-P2-1** | `SpineGpuImportTest`（新增段）或新 target `SpineGpuDeformResolveTest` | 对**每条** deform 时间轴 | 预解析表 `{slot_index, kind, key, key_alt}` 与"逐条走老解析路径"的结果**逐字段相同**；三类 kind 与 inheritDeform（`deform_parent`）都要覆盖 | 预解析写错了不会崩，只会静默作用于错误的槽位 |
| **T-P2-2** | 同上（新 fixture） | 一个骨架，槽位在 **attachment 时间轴**上切 3 个附件（其中两个是 inheritDeform 父子） | 每个切换帧上，deform 内容与"关掉缓存"跑出来的**逐 float 相同** | 版本号失效的直接验证；漏了 ⇒ 附件切走了 deform 还在写 |
| **T-P2-3** | 同上（复用 `SpineGpuSkinFixture` 的思路） | `setSkin` 到**同名但不同皮肤**的附件 | deform **必须被清**（参考实现行为）；缓存版本号必须失效 | 只按名字做版本号会在这里静默错 |
| **T-P2-4** | `SpineGpuImportTest` 新增段 | `di.deform_parent == rec.attachment_id`（继承 deform）的资产 | 父附件的 deform 生效；`di.attachment_id` 直接命中的也生效；两者都不中的**不生效** | `key / key_alt` 两个都容易写反 |
| **T-P2-5** | 同上 | 槽位没挂目标附件 | `slot.deform` **不被写**，且 **size 不变**（连 `resize` 都不许有） | 早退路径上的副作用最容易被快路径漏掉 |
| **T-P2-6** | `SpineGpuRefParityTest`（手动） | 781 资产 | `verdict: PASS`、`分叉的资产 0` | 总闸门 |
| **T-P2-7** | `SpineGpuHotLoopTest`（新增场景） | 动画 + 骨架 + **每帧切附件** + deform | 稳态**每帧 0 次堆分配** | 缓存容器一旦每帧重建就会退化成 malloc |
| **T-P2-8** | `SpineGpuBenchmark`（新建 Release 二进制，env 开关） | `GPUSPINE_DEFORM_RESOLVE_CACHE=0/1` | 交错跑 ≥ 3 轮，看**我们侧 anim 绝对耗时**的差 | < 2% 视为噪声 ⇒ 那就别合 |

---

## 3. P3："其余" 391.9 µs（32.3%）

### 3.1 Step 1：先量，别猜（这一步本身就是 P3 的一半）

"其余"是**没拆开**的一坨（末帧分支 / 早帧分支 / `SetFixedSize`）。加临时计数器（测完即删），
在同一批 781 资产上跑，报四个数：

1. `frame_count == 1` 的 deform 时间轴：**条数** 与 **条次占比**
2. 末帧分支 / 早帧分支 / `SetFixedSize` **各自**的耗时
3. `SetFixedSize`（= `resize`）里有多少次**真的**改变了 size（其余是纯 no-op，可以整条省掉）
4. 单帧时间轴里 rigid / weighted 的比例

**判定**：单帧若占多数 ⇒ 上 §3.2 的快路径；否则按量出来的大头重新排序。
⚠️ 计数器的每一列都要先确认"它是哪两个量相减" —— 之前就因为读错一列，把结论写过一版反的。

### 3.2 Step 2：单帧直写快路径（条件成立时）

单帧时间轴 `frame_count == 1` ⇒ `FirstTime() == LastTime()` ⇒ `time >= LastTime()` **恒真** ⇒
**永远走末帧分支**。而末帧分支要的 `last`（第 0 帧的展开结果）**只依赖 (资产, tl_index)**，
是个常量 ⇒ **缓存一次**，之后每帧直接用，省掉那条时间轴的 `ExpandDeformFrame`
（memset + memcpy + setup 累加）。

- 缓存放哪：优先 `SpineGpuRuntimeData`（按资产，所有实例共享）；它是"不随帧变"的派生数据，
  但要**懒加载**且要注意并发（多实例/多线程首次填充）。保守做法是 per-instance 懒加载。
- ⚠️ **内存必须先数再决定**：单帧全量稠密化 = 条数 × 61.6 float × 4 B（约 246 B/条）。
  **先把 §3.1 的第 1 个数跑出来**；全量太大就只缓存实际播放到的那部分，或加 LRU。
  （对照：P4 全量稠密化实测 **96 MB**，被判不划算。）
- 后续仍要走 blend / alpha 的循环，只省"展开"这一段。

### 3.3 测试用例

| 编号 | 归属 | 输入 / 夹具 | 断言 | 为什么值得钉 |
|---|---|---|---|---|
| **T-P3-1** | `SpineGpuBenchmark`（临时计数器，或 `--csv` 新增列） | 781 资产 | 打印 §3.1 那四个数 | 这一步是**测量**，不是断言；没有它后面全是猜 |
| **T-P3-2** | 新 target `SpineGpuDeformFrameCountTest`（进 ctest） | fixture 覆盖 `frame_count == 1` × {`alpha == 1`, `alpha < 1`} × {kSetup, kFirst, kReplace, kAdd} × {rigid, weighted} × {末帧, 早帧, 插值} | 快路径与**通用路径**的结果**逐 float 相同** | "单帧 ⇒ 永远末帧"是个推论，必须钉死；混合语义一多最容易漏 |
| **T-P3-3** | 同上 | `frame_count == 2` 且两帧 time 相同的退化情形 | 走通用路径，**不**进快路径 | 防止把"帧数 == 1"和"时间跨度为 0"搞混 |
| **T-P3-4** | `SpineGpuRefParityTest`（手动） | 781 资产 | `verdict: PASS`、`分叉的资产 0` | 总闸门 |
| **T-P3-5** | `SpineGpuHotLoopTest` | 单帧 deform + 换肤 + 裁剪 | 稳态每帧 0 分配（缓存数组只能分配一次） | 懒加载最容易在热路径里偷偷分配 |
| **T-P3-6** | `SpineGpuBenchmark`（env 开关） | `GPUSPINE_DEFORM_SINGLE_FRAME_CACHE=0/1` | 交错跑 ≥ 3 轮，看我们侧 anim 绝对耗时 | 同上，< 2% 就是噪声 |

---

## 4. 做完之后的顺序建议

1. **P3-Step1（只加计数器，不改行为）** —— 最便宜，先把"其余"那坨拆开，避免 P2/P3 顺序赌错。
2. **P2**（214.8 µs 已经定位清楚，收益 −80~150 µs，风险集中在版本号覆盖面上）。
3. **P3-Step2**（视 Step 1 的数决定做不做、缓存多大）。
4. 若做完 P2+P3 anim 仍明显 < 0.95x，再回头考虑 P4（载入期稠密化，96 MB），但那已经是在
   **改格式取舍**（§5.2 只存增量省 59.5% 载荷），要先跟格式一起评审。
