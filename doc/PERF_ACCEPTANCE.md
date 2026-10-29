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
| anim | **0.80~0.83x**（仍比参考慢，根源是 deform） |
| pose | ~0.82~0.86x |
| geo | **≥ 2.3x**（我们更快，这块的立身之本） |
| total | **1.43~1.46x**（我们更快） |
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

- **P2**：解析 + 早退 **214.8 µs**（每帧进来 ~43 条 deform、只有 ~17 条真干活）
- **P3**："其余" **391.9 µs**（先量 `frame_count == 1` 的比例，再定快路径）
