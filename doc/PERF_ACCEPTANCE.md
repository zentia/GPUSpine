# GPU-Spine 性能验收文档（deform 热路径，2026-10-29）

## 0. 这份文档管什么

只记**已经做完、并且有可复现验收命令**的东西 —— deform 热路径上的两轮优化，以及一个
**做完了但被实测否决**的方案（P1）。每一项都给出当时的原始数字和现在的验收命令。

| 不管什么 | 去哪看 |
|---|---|
| 完整的推导过程、按时间轴类型的贡献分解、deform 内部拆分 | ZEngine 侧 `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` §17 / §17.2 / §17.4 |
| **还没做**的 P2 / P3 方案与测试用例 | 本仓库 `doc/P2_P3_PLAN.md` |
| 容器格式本身 | ZEngine 侧 `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` |

⚠️ **读数字前先看 §1.3**：这份仓库的绝对值会漂 50%，**只有比值能跨会话比**；而且比值里
"参考侧"自己也在漂，判**我们这侧**的收益要看 **我们侧的绝对耗时**。

---

## 1. 结论一览

### 1.1 三件事的最终结论

| # | 事 | 结论 | 量 | 状态 |
|---|---|---|---|---|
| A | **第一批 `__restrict`**：`ExpandDeformFrame` 的 setup 累加 + `ApplyDeform` 主插值循环 | **快 3.0~4.7%** | anim 比值 0.70x → 0.82/0.82/0.83x；total 1.35x → 1.45~1.46x | ✅ 已合入 |
| B | **第二批 `__restrict`**：全模块扫描（末帧分支 / `alpha != 1` 分支 / `time < FirstTime()` / `ClipTriangles`） | **快 ~0.3%**（噪声边缘） | 我们侧 anim 合计 −0.18% / −0.34%（两轮） | ✅ 已合入，但**别再指望这条线出量** |
| C | **P1 融合快路径**：`alpha == 1` 时不物化 prev/next，一趟从"稀疏帧 + setup"直写 | **慢 1.7~3.5%** | 访存 8.01 → 3.18 次/float（**降了 60%**）却更慢 | ❌ **已做并否决**，代码已删 |
| D | **载入期预解析** `GetFrames()` + 曲线查表（§1.6） | **anim 快 16%** | anim 我们/参考 1.293x → 1.088x 慢；total 1.383x → 1.532x 快；代价 +9.75 MB CPU 共享 | ✅ 已合入 |

**C 的教训（最值钱的一条）**：这段不是带宽受限，是 **SIMD 受限**。展开那三趟
（`assign` 补 0 / `memcpy` 稀疏段 / 逐点加 setup）都是连续直线循环、能向量化；融合之后
逐元素要判"i 落不落在稀疏区间里"，循环带分支、只能标量跑。**省访存不等于省时间。**

### 1.2 deform 内部的账（为什么只剩 P2/P3 可做）

两侧同时消融实测：**deform 一条占满 anim 段全部差距的 100.2%**（其余类型净贡献 ≈ 0，略负）。
deform 内部拆开（我们这侧，同一资产上 A/B 两遍）：

| 去掉的部分 | Δ µs | 占 deform |
|---|---|---|
| 整条 deform 跳过 | 1212.1 | 100% |
| ├ **解析 + 早退**（只解析、不干活）→ **P2** | **214.8** | 17.7% |
| └ 展开之后的顶点工作 | 997.3 | 82.3% |
| 　 ├ 稀疏展开（memset + memcpy + setup 累加） | 239.8 | 19.8% |
| 　 ├ 展开之后的逐顶点混合循环 | 365.6 | 30.2% |
| 　 └ **其余（末帧 / 早帧 / `SetFixedSize`）→ P3** | **391.9** | 32.3% |

⚠️ **2026-10-29 修订**：上面这份 17.7% 是"改前改后各跑一遍"的消融估计值（见 §1.3 第 4 条）。
改用**逐帧交错**（同一趟跑里交替切档）重测后，**解析 + 早退只有 102.5 µs = 我们 anim 的 3%**（§1.4）
⇒ **P2 的收益上限按新数字算，别再按 17.7% 排优先级**。

分支画像（真正干活的 6,163,597 条次里）：`alpha == 1` **84.3%**、`blend == kAdd` **0.0%**、
走末帧分支 **26.1%**（单帧时间轴会永远走这条）、rigid **67.7%**、平均 deform 长度 **61.6 float**、
进来 15,240,246 条次只有 **40.4%** 真干活（其余在"槽位此刻没挂这个附件"上早退）。

### 1.3 ⚠️ 量法（不看这条会得出反的结论）

1. **绝对值漂 50%（两边一起漂）⇒ 只能看比值。** 拿这份数字跟以后比之前，先连跑三次取最闲的那次。
2. **比值也会骗人**：它是"参考/我们"，参考侧自己在漂。判**我们这侧**的收益必须看
   **我们侧的绝对耗时**。第一轮扫描时比值看着从 0.79 涨到 0.82（像 +4%），其实是参考侧
   3636.8 → 3767.9 在漂，我们侧只动了 −0.18%。
3. **低于 ~2% 的效果只能靠"同一份二进制 + 开关 + 交错跑"量出来**：改前改后各跑三次是噪声。
   这份仓库的 A/B 用的是**同一份源码树编两份二进制、交错跑**（只差被测的那个改动）。
4. **消融（把一段代码跳过）估出来的值偏大 ~2 倍** ⇒ 消融只用来找"有没有"，定收益要交错跑。

### 1.4 anim / pose 两段"慢"的拆账（2026-10-29，逐帧交错消融）

**方法**：同一份二进制、同一趟跑里**逐帧交错切档** —— deform 三档（全开 / 只解析 / 整条跳过）
× pose 四档（全开 / 跳第 7 步 / 跳第 8 步 / 两步都跳），周期 12 帧；两侧按**资产交替**跑。
机器漂移在档位之间互相抵消。

```bash
SpineGpuBenchmark.exe <assets> --ab anim --frames 240   # 只消融 deform（pose 原样）
SpineGpuBenchmark.exe <assets> --ab pose --frames 240   # 只消融 pose（deform 原样）
```

⚠️ **复现方法**：`--ab` / `--ab-type` 都是**临时插桩**（`GPUSPINE_AB`：我们侧按类型早退、
参考侧把某一类时间轴换成空替身 `NullTimeline`、benchmark 里逐帧切档）。
这两轮量完**已经删掉**（产品库不留开关）。要重测按 §1.4 / §1.5 的档位描述临时加回来，
并且**必须带上 §1.5 那三个坑的处理**（中位数栏 / 混音窗口不计数 / 哑档常量底）。

⚠️ **副作用（这条决定了哪些数字能信）**：切档会改变整趟的总工作量 ⇒ 改变机器的工作点
（墙钟 8.4 s vs 不带消融的 20.3 s），**两侧比值跟着变**（anim 差距 613 µs vs 基线 720~880 µs）。
所以：**段内占比可以信（档位自己交错），跨侧差距只能看不带消融的基线。**

#### [anim] `--ab anim`（两次相差 0.6%）

| 项 | 我们 µs | 占我们 anim | 参考 µs | 占参考 anim |
|---|---|---|---|---|
| 非 deform（状态机 + 其余类型时间轴） | 2211.6 | **63%** | 1846.1 | 64% |
| deform 真干活（稀疏展开 + 逐顶点混合 + 末帧分支） | 1181.5 | 34% | 1036.6 | 36% |
| **deform 解析 + 早退** | **102.5** | **3%** | （含在 deform 那行里） | |
| 合计 | 3495.5 | 100% | 2882.7 | 100% |

差距 612.8 = 非 deform **+365.5（60%）** + deform 干活 +144.9（24%）+ 我们的解析 +102.5（17%）。

#### [pose] `--ab pose`

| 项 | µs | 占我们 pose |
|---|---|---|
| 剩下（1~6 步：骨骼输出 + IK / transform / path 约束 + path 前置） | 665.2 | **68%** |
| 第 7 步 槽位输出（颜色 / 附件 → GPU 采样） | 94.8 | 10% |
| 第 8 步 槽位→绘制项 + mesh deform 打包 | 224.8 | 23% |
| 合计 | 984.7 | 100%（参考 755.4） |

⚠️ 一个反直觉的细节：**两侧 deform 在 anim 里的占比几乎相同**（我们 36.7% / 参考 36.0%）。
所以不是"deform 特别差"，而是**我们 anim 整体比参考慢 ~20%**：
deform 那块慢 24%（1284 vs 1036.6）、非 deform 那块慢 20%（2211.6 vs 1846.1）——**按比例均匀慢**。
（不带消融的基线 idle 也是这个数：我们/参考 = 1.15x / 1.16x。）

#### 三条结论

1. **anim 差距的 60% 在"非 deform"那 63%，但这不是因为它特别差** —— 它是块头大
   （2211.6 µs，比参考慢 20%），deform 那块也慢 24%。
   ⇒ 下一步拆"整体慢 20%"这件事（Update vs Apply、按时间轴类型、状态机记账），**不是**先做 P2。
2. **"解析 + 早退"只值 102.5 µs（我们 anim 的 3%）**，比 §1.2 那份 17.7% 小一个量级；
   **deform 对 anim 差距的贡献也从 §1.2 的 100.2% 降到 40%**（旧数字是在机器带负载时取的：
   参考侧访存更重、负载下退化更多，且用的是非交错消融）。
   ⇒ **P2（预解析表）从"主要收益"降级为"顺手做"**；deform 干活那 34% 是"只存增量"的
   固有成本（P1 已证明融合快路径反而更慢），**不动**。
3. **pose 不是慢，是口径不同**：可比的那块（1~6 步）665.2 µs 比参考整个 pose（755.4）还快
   **1.14x**；多出来的 319.6 µs（第 7/8 步）参考算在 **geo 段**（它遍历 drawOrder +
   computeWorldVertices）。**pose+geo 合计：我们 4069.6 vs 参考 7730.8 ⇒ 快 1.90x。**
   ⇒ 单独比 pose 会得出反的结论，pose 必须和 geo 一起看。

### 1.5 再往下拆一层：anim 的 apply 按时间轴大类（2026-10-29）

**先排除两个半边**：`--split` 白给的 update/apply —— **update 只有 19.4 µs（anim 的 0.5%），
两边一样**（我们 19.4 vs 参考 25.0）⇒ **anim 的差距全在 apply（逐时间轴写状态）**。
两边每帧 apply 的时间轴条数**完全相同**（78.25 条中位，合计 90,218）⇒ 是"每条更贵"，不是"条数更多"。
按平均算：我们 48.6 ns/条 vs 参考 38.8 ns/条。

**按大类消融**（`--ab-type`：我们侧 `SpineGpuApplyTimeline` 按类型早退；参考侧把那一类换成空替身；
周期 17 帧交错）。⚠️ 三个坑，都踩过：

1. **求和口径被少数重资产主导**（机器忙时噪声 ±600~1500 µs）⇒ 加**逐资产中位数**栏，两栏同向才信。
2. **切动画之后有 13 帧在混音**（两遍动画一起 apply，约 2× 开销）⇒ 这段窗口**不计数**。
3. **每档都有一个常量底**：哑档（`skip` = 不存在的类型，一条也没跳）实测我们侧 **+0.36 µs**、
   参考侧 ~0 ⇒ **每一档都要先减掉这个底**。减完之后两个"应该为 0"的对照组确实回到 0：
   `event`（我们侧是空实现）0.00、`twoColor`（全库只有 29 条）−0.03 ⇒ 方法可信。

减完底之后的**逐资产中位数**（µs / 资产 / 帧，三次跑相差 <0.07）：

| 类型 | 我们 | 参考 | 差 | 占差距 |
|---|---|---|---|---|
| **deform** | 0.71 | 0.03 | **0.68** | **39%** |
| translate | 0.30 | ~0.00 | 0.30 | 17% |
| **rotate** | 0.47 | 0.18 | 0.29 | 17% |
| color | 0.24 | 0.05 | 0.19 | 11% |
| attachment | 0.23 | 0.08 | 0.15 | 9% |
| scale | 0.10 | −0.04 | 0.14 | 8% |
| 其余（event / drawOrder / ik / transform / path* / twoColor / shear） | ≈0 | ≈0 | ≈0 | — |
| 合计 | | | **1.75** | （基线均差 1.86） |

**结论**：**没有单一元凶** —— deform 是最大的一块（39%，与 §1.4 的 40% 对得上），
其余按"条数多少"均匀铺在 translate / rotate / color / attachment / scale 上，
与 §1.4 "整体慢 ~20%" 完全一致 ⇒ **真问题是"每条时间轴的 apply 比参考贵"（48.6 vs 38.8 ns/条）**，
是**按条计价**的开销，不是某一类时间轴的算法问题。

**下一个可做的**（也是最像能出量的）：把 `GetFrames()` 的偏移计算与曲线查表从"每帧每条重算"
改成**载入期预解析进 `SpineGpuRuntimeData`**（每条时间轴直接拿到 `const float* frames` +
帧数 + 步长），让 apply 的第一件事是"读一个指针"而不是"算三个偏移"。
代价是运行时数据大一点，收益面覆盖**所有类型**。
⚠️ 动之前先按 §1.4/§1.5 的方法重测一遍拿准数（绝对值随机器负载漂，只能看比值与中位数）。

**⇒ 已做，见 §1.6。**

---

### 1.6 载入期预解析 `GetFrames()` + 曲线查表（2026-10-29，已合入）

**做了什么**：新增 `SpineGpuTimelineRt`（`SpineGpuAnimSolver.h`），`SpineGpuRuntimeData::timelines`
与 `asset.timelines` **一一对应**，载入期（`SpineGpuBuildRuntimeData`）一次算好并**校验**：

| 字段 | 以前在热路上每次算 | 现在 |
|---|---|---|
| `frames` | `asset.frames.data() + tl.frame_first` | 直接读指针 |
| `entries` | `SpineTimelineEntries(type)`（switch） | 直接读 |
| `target` / `type` | `asset.timelines[tl_index]` 再取一次 | 直接读 |
| 帧区间越界 | `frame_first + frame_count × entries > frames.size()`（64 位乘加 + 两次 `size()`） | 载入期挡住 |
| `curve` / `bezier` | `curve_first + 段号`、`curve_bezier_index` 的 `size()` 相等性、bezier 表区间 | 载入期挡住，热路直接下标 |

`GetFrames()` 与 `FrameView` **已删除**（后者的方法搬进了 `SpineGpuTimelineRt`）。
⚠️ 求值侧**只留**一条检查：`kSpineNoBezierIndex`（这段标了 bezier 但缓存里没表）—— 它是语义，
读下去就是越界；"曲线缓存没建"那条**前移到载入期**，现在表现为 `SpineGpuBuildRuntimeData` 失败
（`SpineGpuImportTest` / `SpineGpuHotLoopTest` 里那两条 `Check(!r.ok)` 已改成建表报错）。

**A/B 方法**：同一台机器、同一目录，**两个 exe 交替跑**（`SpineGpuBenchmark_old.exe` /
`_new.exe`，只差这一笔改动），各跑 4 次（含一次**反序**：new→old，确认不是"跑在后面就更慢"的漂移）。
⚠️ 绝对比值会漂，所以**两侧都在同一个 exe 里** ⇒ 用 **我们/参考 的倍率**（参考侧就是这次运行的漂移标尺）。

| 口径 | 旧（我们/参考） | 新（我们/参考） | 变化 |
|---|---|---|---|
| anim 合计（µs） | 1.285 / 1.301 / 1.291 / 1.285 → 中位 **1.293** | 1.109 / 1.087 / 1.079 / 1.090 → 中位 **1.088** | **−15.9%** |
| anim 逐资产中位 | 1.502 / 1.476 / 1.471 / 1.530 → **1.489** | 1.292 / 1.221 / 1.244 / 1.328 → **1.268** | **−14.8%** |
| pose 合计 | 1.180 | 1.193 | 噪声（代码没动） |
| geo 合计 | 0.446（我们 2.24x 快） | 0.431（我们 2.32x 快） | 噪声（代码没动） |
| **total** | 0.723 ⇒ 我们 **1.383x 快** | 0.653 ⇒ 我们 **1.532x 快** | |

⇒ **anim 从"比参考慢 29%"降到"比参考慢 9%"**（我们这侧 anim 合计 ≈ 4000 → ≈ 3400 µs）。
两个口径（合计 / 逐资产中位）差 1 个点，同向 ⇒ 按 §1.5 的判据可信。

**代价**：`SpineGpuTimelineRt` 是 **40 B/条**（估的是 16 B，实测每条多了 `curve` / `bezier`
两个指针）。781 个资产 CPU 共享内存 **343792.6 → 353776.8 KB = +9.75 MB**（+2.9%，
逐资产中位 229.22 → 238.82 KB，≈ 320 条时间轴/资产）。仍然 **1.17x 小于参考**（旧 1.20x），
N=1/10/100 三个档我们依旧处处更少 ⇒ 这笔换算是值的。

**已知限制**：`SpineGpuRuntimeData` 现在持有**指进 `SpineGpuAsset` 内部**的裸指针
（`frames` / `curve` / `bezier`）⇒ 建完表之后那个 asset **不能**再让 `frames` /
`curve_types` / `curve_bezier_index` 重新分配（**移动** asset 是安全的，`vector` 搬家不动堆元素）。
这点写进了 `SpineGpuTimelineRt` 的注释。

**剩下的（anim 还差 9%）**：`SpineGpuCurvePercentAt` 里的 `segment = cur / entries - 1`
仍是**运行时整数除法**（`entries` 是 1/2/3/5/8，但编译器不知道 ⇒ 真 `udiv`，~20 周期）。
下一步可以按 `entries` 分派成常量除法（`switch`），或者直接存"段号表的字节步长"。
另外 deform 仍是最大的单块（§1.5 的 39%），但它现在是**按条**贵而不是算法问题。

---

## 2. 环境与输入

| 项 | 值 |
|---|---|
| 资产集 | `G:/OSGame/Client_proj/Project/Assets`，**781 个骨架** |
| 参考实现 | spine-cpp **3.8**，`G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp` |
| 测时间 | **Release**（Debug 的 `_ITERATOR_DEBUG_LEVEL=2` 会给容器多发 16 B，分配数只有 Release 能读） |
| 测正确性 | **Debug** |
| 夹具用的 json 解析 | 只测试用（`GPUSPINE_ENABLE_TEST_JSON`），产品库不带 json、不依赖 rapidjson |

---

## 3. 已验收项 A：第一批 `__restrict`（+3.0~4.7%）

**改了什么**：`Runtime/GPUSpine/SpineGpuSkeletonState.cpp` 里两处 ——
`ExpandDeformFrame` 的 setup 累加（`dst[q] += rs[q]`）、`ApplyDeform` 主路径的插值循环
（`v[i] = pv[i] + (nv[i] - pv[i]) * percent`）。

**为什么有效**：`vertices`（槽位的 deform 向量）/ `prev` / `next`（thread_local scratch）/
`setup`（指进 `asset.vertex_buffer`）四块数据互不相交，但不写出来 MSVC **不敢向量化**。
访存次数不变（7.89 vs 8.01，差异只是计数口径）。

**原始数字**（变体 0 = 老路径，变体 2 = 老路径 + `__restrict`；同一个资产上 A/B 两遍）：

| 轮 | 变体 0 | 变体 2 | 差 |
|---|---|---|---|
| 1 | 3911.1 µs | 3793.6 µs | **快 3.0%** |
| 2 | 3868.6 µs | 3687.0 µs | **快 4.7%** |

⚠️ 这两轮里变体 0 都是"第二遍"（紧接在变体 2 之后跑，缓存更热）⇒ 3.0~4.7% 是**保守下界**。

---

## 4. 已验收项 B：第二批 `__restrict`（+0.3%，噪声边缘）

**扫描范围** = `Runtime/GPUSpine/` 全部 9 个 .cpp。筛选标准：**有没有第二块指针** +
**循环体内有没有 `break` 之类的边界检查**（有的话 MSVC 本来就不向量化，加了也白加）。

| 位置 | 循环 | 处理 |
|---|---|---|
| `ApplyDeform` 主插值（`alpha == 1` ⇒ 73.9%） | `vr[i] = p + (nvr[i]-p)*percent` | A 轮已加 |
| `ExpandDeformFrame` 的 setup 累加（rigid 67.7%） | `dst[q] += rs[q]` | A 轮已加 |
| `ApplyDeform` **末帧分支**（26.1%） | kSetup/kAdd × rigid/weighted、kFirst/kReplace 共 6 条 | **B 轮加** |
| `ApplyDeform` `alpha != 1` 分支（15.7%） | kSetup/kAdd × rigid/weighted、kFirst/kReplace 共 4 条 | **B 轮加** |
| `ApplyDeform` `time < FirstTime()` 的 rigid 循环 | `v0[i] += (s0[i]-v0[i])*alpha` | **B 轮加**（罕见） |
| `SpineGpuClipper::ClipTriangles` 输出循环 | `cv` / `cuv` / `co` | **B 轮加**（只在裁剪时跑） |
| `PoseSolver` 的 path / mesh 顶点循环 | 体内有 `vi+1 >= send` 之类 `break` | **不加** |
| `AnimSolver` 的贝塞尔表 / 帧查找 | 单指针 + 定长 9 次 | **不加** |

**原始数字**（同一份源码树编两份二进制，交错跑；看**我们侧绝对值**）：

| 轮 | base 我们 anim 合计 | restrict 我们 anim 合计 | 差 |
|---|---|---|---|
| 1 | 4593.0 µs | 4584.7 µs | **−0.18%** |
| 2 | 6408.6 µs | 3647.7 µs | 作废（整轮绝对值翻倍，机器被别的事占着） |
| 3 | 3320.2 µs | 3308.8 µs | **−0.34%** |

⇒ **~0.3%**，方向两轮一致，但远低于"2% 才可信"的阈值。

**还是留下的理由**：这几条 `__restrict` 断言的都是**真命题**（代码注释里逐条写明了每块数据的
来源），零运行成本、零行为差异，顺手把"这两块数据不会重叠"这个事实固化进代码。

### ⚠️ 写 `__restrict` 的两个野指针坑（B 轮踩到，全工程通用）

1. **指针必须在最后一次可能重分配的操作之后取。** `last` / `prev` / `next` 是
   `ExpandDeformFrame` 里 `full.assign()` 的目标，容量不够就重分配 —— 提前取的指针会悬垂。
   `assign` 只在容量不足时重分配，所以**稳态"看起来没问题"**，这正是最难复现的那类 bug
   （预热期、或遇到更长的 deform 时间轴时才炸）。
2. **同一个对象只能有一份 `__restrict` 指针。** 原代码在主插值那个块里又声明了一遍
   `v` / `pv` / `nv`（和外层 `vr` / `pvr` / `nvr` 指向同一块内存）—— 两个 restrict 指针
   指向同一块内存，"基于 restrict 的访问"这条约定自己就破了。已改成复用外层。

---

## 5. 已否决项 C：P1 融合快路径（−1.7~3.5%）

`alpha == 1` 且覆盖语义（84.3%）时不再物化 prev/next 两份稠密数组，一趟从"稀疏帧 + setup"
直写 `slot.deform`。**访存真的降了 60%**（8.01 → 3.18 次/float，临时计数器实测），同一个资产上
A/B 两遍（间隔 ~14 ms ⇒ 漂移抵消）：

| 轮 | 老路径 | 融合 | 差 |
|---|---|---|---|
| 1 | 4122.4 µs | 4248.5 µs | 融合**慢 3.1%** |
| 2 | 4064.0 µs | 4208.2 µs | 慢 3.5% |
| 3 | 3893.5 µs | 3959.6 µs | 慢 1.7% |

**为什么**：逐元素要判 `i` 在不在稀疏区间 ⇒ 循环带分支、只能标量跑；而展开那三趟每一趟都是
**连续直线**循环、能向量化。省的是访存，亏的是 SIMD。

融合快路径的代码、临时开关、访存计数器**已全部删除**，结论留在这一节和 §1.1。
（P4 载入期稠密化实测要 **96 MB**，也不划算 —— 见 ZEngine 侧 §17.4。）

---

## 6. 验收清单

### 6.1 configure（只需一次）

```bash
cd <本仓库根>
cmake -S . -B build \
  -DGPUSPINE_BUILD_TESTS=ON \
  -DGPUSPINE_RAPIDJSON_DIR="<含 rapidjson/document.h 的 include 目录>" \
  -DGPUSPINE_REF_ROOT="G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp"
```

末尾必须出现 `GPUSpine: 参考对拍已注册`；否则后面两条对拍/基准目标不会生成。
`GPUSPINE_RAPIDJSON_DIR` 可以指向 ZEngine 的 `engine/3rdparty/rapidjson/include`。

### 6.2 正确性（Debug）

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug
```

**期望**：

| 测试 | 断言什么 | 期望 |
|---|---|---|
| `SpineGpuImportTest` | 导入器 / 容器 / 解算器自测 | PASS |
| `SpineGpuHotLoopTest` | **稳态每帧 0 次堆分配**（动画+骨架、动画+骨架+裁剪都必须是 0；四边形裁剪 ≤ 2 次） | PASS |
| `SpineGpuCSharpParityTest` | C++ 写 → C# 读 → C# 写，逐字节相同（**需要装 dotnet**，没装就自动跳过） | PASS |

⚠️ `ctest` 在这套多配置构建下**必须带 `-C Debug`**，否则全 "Not Run"。
⚠️ `SpineGpuHotLoopTest` 整目标设了 `_ITERATOR_DEBUG_LEVEL=0`（MSVC Debug 的迭代器调试会给
每个容器多发一次 16 B，是假阳性），所以**不链别的库**。
⚠️ `SpineGpuCSharpParityTest` 依赖 `SpineGpuImportTest --dump` 打在 stdout 上的 `dump: N`
（CMake 用 `execute_process`、**管道**抓）。MSVC 的 stdout 走管道是全缓冲，那行只有几十字节
填不满缓冲区 ⇒ 不设成不缓冲的话抓取方拿到空串、**落盘其实成功但对拍假红**。
`SpineGpuImportTest::main` 开头已有 `setvbuf(stdout, nullptr, _IONBF, 0)`（与
`SpineGpuBenchmark` 同一招，那边的文件头记着"经 MSYS 管道重定向时缓冲的输出在进程退出时会丢"）。
**改这两个 exe 的输出时别把那句删了。**

### 6.3 与参考实现对拍（Debug，781 资产，约 4 分钟）

```bash
./build/Tests~/Debug/SpineGpuRefParityTest.exe "G:/OSGame/Client_proj/Project/Assets"
```

**期望**：结尾 `=== verdict: PASS ===`，且 `分叉的资产 : 0`。
B 轮改完之后跑的一次：**PASS / 0 分叉**，其中
`deform 逐 float 274,232,712` 个全部一致；`裁剪` 也被覆盖（`clipTriangles` 逐位 359,772 次）
⇒ `ClipTriangles` 那处 `__restrict` 是真跑到了，不是空转。

### 6.4 性能（Release）

```bash
cmake --build build --config Release --target SpineGpuBenchmark
./build/Tests~/Release/SpineGpuBenchmark.exe "G:/OSGame/Client_proj/Project/Assets"
```

**期望**（连跑三次取最闲的那次；绝对值会漂，只看比值）：

| 段 | 期望比值（参考/我们） |
|---|---|
| anim | **0.90~0.93x**（仍比参考慢 ~9%；§1.6 之前是 0.77x） |
| pose | ~0.82~0.86x |
| geo | **≥ 2.3x**（我们更快，这块的立身之本） |
| total | **1.50~1.54x**（我们更快；§1.6 之前是 1.38~1.40x） |
| upload | 恒 2.62x（算出来的，不漂） |
| **稳态每帧分配** | **我们 0 次 / 0 字节** |

⚠️ 目录是**第一个位置参数**。exe 在 `build/Tests~/<config>/` 下（测试目标定义在 `Tests~/`
子目录里，不是 `build/<config>/`；`~` 在 shell 里记得加引号）。

---

## 7. 从零复现（干净机器）

```bash
git clone https://github.com/zentia/GPUSpine.git
cd GPUSpine
cmake -S . -B build -DGPUSPINE_BUILD_TESTS=ON \
  -DGPUSPINE_RAPIDJSON_DIR=<rapidjson include> \
  -DGPUSPINE_REF_ROOT=<spine-cpp 3.8 根>
cmake --build build --config Debug && ctest --test-dir build -C Debug
cmake --build build --config Release --target SpineGpuBenchmark
./build/Tests~/Release/SpineGpuBenchmark.exe <Spine 资产目录>
```

只验核心库（不测、不对拍）的话更简单，十几秒：

```bash
cmake -S . -B build && cmake --build build --config Release --target GPUSpine
```

---

## 8. 下一步

`__restrict` 这条线**到此为止**（剩下能加的都加完了，合计只值 ~0.3%）。
anim 段剩下的真问题只有两个，方案与测试用例见 **`doc/P2_P3_PLAN.md`**：

- ~~**P2**：解析 + 早退 **214.8 µs**~~ → **2026-10-29 修订：只有 102.5 µs（anim 的 3%）**，降级（§1.4）
- ~~**P3**："其余" **391.9 µs**~~ → 同上，按新数字重排
- **新 1 号目标**：anim 的"非 deform"那 63%（2211.6 µs，比参考慢 20%）⇒ 先拆 Update / Apply 与按时间轴类型
- **新 2 号目标**：pose 的账要和 geo 合起来看（§1.4 结论 3），单独比 pose 会得出反的结论
