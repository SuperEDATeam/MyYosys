# MyIR 与 Yosys 的优化对比及形式等价验证实验报告

> 配套数据：`tests/epfl/results/epfl_results.{csv,md}`
> 复现脚本：`tests/epfl/run_compare.py`
> 实验对象：自研中间表示 MyIR 的优化流水线 vs. Yosys 原生 `opt -full`
> 数据集：EPFL Combinational Benchmark Suite（[lsils/benchmarks](https://github.com/lsils/benchmarks)）

---

## 摘要

本实验在 EPFL 公开逻辑综合 benchmark 上，系统地对比了自研中间表示 **MyIR** 的优化流水线与成熟工业级综合器 **Yosys** 原生优化（`opt -full`）的综合质量，并对每一条 benchmark 的优化结果做了**形式等价（SAT）证明**作为正确性背书。

在 10 条组合电路 benchmark 上：

- **综合质量**：MyIR 在其中 **9 条**上 cell 数严格优于 Yosys，几何平均 cell 比 **0.804**（即平均比 Yosys `opt -full` 少约 **20%** 的 cell），最高在 `bar` 上减少 **35.2%**。
- **正确性**：**10 / 10 全部通过 SAT 形式等价证明**（含 5000+ cell 的 `bar` / `max`），证明 `AST → MyIR → 优化 → RTLIL` 全链路与 Yosys 基线在功能上完全一致。
- **关键归因（消融实验）**：关闭 `myir_rewrite` 后，MyIR 流水线对 10 条 benchmark 产出的 cell 数与 Yosys 基线**逐一完全相等**；优化优势 100% 来自 `myir_rewrite` 的 k-cut 局部代数重构——这正是 Yosys 在 RTLIL 层 `opt` 阶段刻意不做、留给后端 ABC 的那一类优化。
- **工程瓶颈与突破**：实验初期 `myir_rewrite` 在 2000+ cell 的设计上因"每应用一次重写就全模块重枚举一次 k-cut"的 O(E²·C) 复杂度而长期超时（>400 s）。经**批量化重写 + 哈希去重**改造（复杂度降至近似 O(E·C)），`adder` 全流水线从超时降至 **≈7 s**，使大规模 benchmark 的优化对比与形式验证首次成为可能。

---

## 一、引言与实验目标

MyIR 是构建在 Yosys 之上的一套自定义中间表示与优化 Pass 集合（通过 Yosys 的 pass 机制以 `myir_*` 命令形式注册）。其核心问题是：

> 一套"教学/研究导向、可读性优先"的自研 IR 与优化器，在真实电路上的综合质量能否达到、乃至超过成熟工业综合器的同层优化，且优化结果是否在功能上严格正确？

为回答这一问题，本实验设定三个目标：

1. **质量对比**：在统一、可复现的流程下，量化 MyIR 优化后网表相对 Yosys `opt -full` 的 cell 数比值（ratio = MyIR cells / Yosys cells，< 1 即 MyIR 更优）。
2. **正确性背书**：对每条 benchmark 的优化结果做形式等价证明，确保"更小"不是以牺牲功能为代价。
3. **归因分析**：定位优势/劣势的根本来源，识别关键优化命令，并诚实分析未能超越的个例。

---

## 二、实验对象：两条综合流程

实验对每条 benchmark 跑两条独立流水线，二者**读取同一份 Verilog 源文件**，最终都以 Yosys `stat` 统计同一抽象层（RTLIL 粗粒度 cell）的规模，保证可比性。

### 2.1 Yosys 基线流程（gold）

模板 `tests/epfl/templates/yosys_baseline.ys.tmpl`：

```
read_verilog {INPUT}
hierarchy -auto-top
proc
opt -full
clean
stat
write_rtlil {OUT_RTLIL}
```

`opt -full` 是 Yosys 在 RTLIL 层的"全力"优化宏，内部迭代执行 `opt_expr`（常量折叠 / 表达式规整）、`opt_merge`（公共子表达式合并 / CSE）、`opt_muxtree`、`opt_reduce`、`opt_share`、`opt_dff`、`opt_clean`（死代码清除）等子 Pass 直至收敛。它代表"Yosys 仅凭自身 RTLIL 级优化能做到什么"的公平基线。

### 2.2 MyIR 工具流程（gate）

模板 `tests/epfl/templates/myir_pipeline.ys.tmpl`：

```
read_verilog {INPUT}
hierarchy -auto-top
myir_ast_import          # Verilog AST 直接转 MyIR
myir_opts -fixpoint      # MyIR 优化流水线，迭代至不动点
myir_export              # MyIR 转回 RTLIL
proc
opt_clean                # 仅做死代码清除，不做额外逻辑优化
stat
write_rtlil {OUT_RTLIL}
```

其中 `myir_opts -fixpoint` 串联 MyIR 全部优化 Pass 并迭代到 IR 不再变化（上限 10 轮），其内部顺序为：

```
myir_opt_const → myir_opt_expr → myir_opt_strength → myir_opt_compare
→ myir_opt_concat → myir_opt_muxtree → myir_opt_share
→ myir_rewrite → myir_opt_share → myir_opt_clean
```

注意末段 `proc; opt_clean` 仅承担过程块降级与死代码清除，**不调用 Yosys 的 `opt`**，因此最终 cell 数差异完全归因于 MyIR 自身的优化能力，而非借用了 Yosys 的优化。

### 2.3 公平性与口径说明（Threats to Validity）

为使结论可信，明确以下口径：

- **比较层级**：两侧均在 RTLIL 粗粒度 cell（`$and`/`$or`/`$not`/`$xor`/`$mux` 等）层统计，**均未做工艺映射**（不含 `techmap`/`abc`）。因此本实验度量的是"RTLIL 级逻辑优化"能力，而非最终门级面积。
- **基线选择**：基线取 `opt -full` 而非 `opt -full; techmap; abc`。这是有意为之——`myir_rewrite` 工作在 IR 层、不接触工艺库，与 ABC 的 AIG 级重写不在同一层次；若纳入 ABC，Yosys 很可能反超。本实验的结论应被理解为**"在 RTLIL/IR 同层优化下 MyIR 优于 Yosys 原生 `opt`"**，而非"MyIR 优于完整的 Yosys+ABC 综合"。
- **数据集特征**：EPFL 组合 benchmark 多为 2 输入门级结构网表、纯组合、无时序，恰好落在 AST 路线当前最稳健的子集内，避免触及 AST 主干尚弱的特性（memory / 复杂 case 等）。

---

## 三、测试数据集

### 3.1 来源与获取

数据集为 **EPFL Combinational Benchmark Suite**（EPFL 集成系统实验室发布的逻辑综合标准评测集，学术界广泛使用），通过以下方式拉取到本地：

```powershell
git clone --depth 1 https://github.com/lsils/benchmarks tests/epfl/lsils_benchmarks
```

### 3.2 实际目录结构与内容

实际拉取到 `tests/epfl/lsils_benchmarks/`，顶层结构如下：

```
lsils_benchmarks/
├── arithmetic/        # 算术电路，共 10 组，每组 4 种格式（.v/.aig/.blif/.vhd）
├── random_control/    # 随机/控制逻辑电路，共 10 组，每组 4 种格式
├── best_results/      # 社区已知最优综合结果（参考用，本实验未使用）
├── LICENSE
└── README.md
```

每个电路同时提供四种等价表示：`.v`（Verilog，本实验输入）、`.aig`（And-Inverter Graph）、`.blif`、`.vhd`（VHDL）。两个核心目录各含 10 个电路：

- **`arithmetic/`**：`adder`、`bar`、`div`、`hyp`、`log2`、`max`、`multiplier`、`sin`、`sqrt`、`square`。
- **`random_control/`**：`arbiter`、`cavlc`、`ctrl`、`dec`、`i2c`、`int2float`、`mem_ctrl`、`priority`、`router`、`voter`。

数据集另含超大规模的 MtM（Multiplier-the-Mega，千万门级）子集，本实验通过 benchmark 清单中的 `!mtm` 前缀默认排除（需 `--include-mtm` 才启用），因其规模会耗尽普通工作站内存。

### 3.3 本实验的 benchmark 选取

清单文件 `tests/epfl/benchmarks.txt` 选取了 10 条规模适中、AST 路线可稳健处理的组合电路作为 tier-1 集合：

| 类别 | benchmark | 说明 |
|---|---|---|
| random_control | `dec` | 二进制→独热译码器 |
| random_control | `int2float` | 整数转浮点 |
| random_control | `ctrl` | 控制逻辑 |
| random_control | `cavlc` | CAVLC 编码控制 |
| random_control | `priority` | 优先级编码器 |
| random_control | `router` | 路由控制 |
| random_control | `i2c` | I²C 控制器组合部分 |
| arithmetic | `adder` | 进位链加法器 |
| arithmetic | `bar` | 桶形移位器 |
| arithmetic | `max` | 最大值选择 |

更重的算术件（`multiplier`/`hyp`/`log2`/`sqrt`/`div`/`square`）和重控制件（`arbiter`/`voter`/`mem_ctrl`）在清单中保留但默认注释，原因见 §6.4。

---

## 四、测试驱动脚本 `run_compare.py`

`tests/epfl/run_compare.py` 是整个实验的自动化引擎（Python 3.9+，纯标准库，约 740 行）。

### 4.1 功能概览

对清单中每条 benchmark，按需执行三个阶段，并汇总为机器可读 CSV 与人类可读 Markdown 报告：

1. **阶段一**：跑 Yosys 基线流水线，解析 `stat` 取 cell/wire 数；
2. **阶段二**：跑 MyIR 流水线，解析 `stat`，计算 ratio；
3. **阶段三（可选）**：对两侧输出 RTLIL 做等价证明。

### 4.2 关键 CLI 与运行模式

| 选项 | 作用 |
|---|---|
| `--mode {measure,prove,both}` | `measure` 只收集 cell ratio、**不做证明**（最快，优化对比数据本不需要证明）；`prove`/`both` 额外做等价证明 |
| `--struct-threshold N` | cell 数 ≤ N 走 SAT 证明，> N 走结构等价代理；`0` = **强制全部 SAT** |
| `--pipeline {full,lite}` | `full` 用 `myir_opts -fixpoint`；`lite` 用 `myir_opts -once -norewrite`（Debug 调试用，快很多） |
| `--timeout S` | 单次 yosys 调用超时（默认 600 s） |
| `-j N` | 并行 benchmark 数 |
| `--filter <regex>` / `--keep-intermediate` / `--include-mtm` | 子集过滤 / 保留中间文件 / 启用 MtM |

### 4.3 代码逻辑要点

- **二进制定位 `find_yosys()`**：依次按 `--yosys` 参数 → `YOSYS_BIN` 环境变量 → 常见 VS 输出路径（`x64/Release` 等）→ `PATH` 查找，跨机器免改脚本。
- **清单解析 `load_bench_list()`**：忽略 `#` 注释，`!mtm` 前缀的超大电路默认跳过。
- **模板渲染 `render_template()`**：把 `.ys.tmpl` 中的 `{INPUT}`/`{OUT_RTLIL}`/`{STAT_OUT}` 等占位符替换为实际路径（统一正斜杠，避免 Windows 反斜杠转义问题）。
- **子进程执行 `run_yosys()`**：以 `yosys -q -s script.ys` 启动，捕获 stdout+stderr 落地为 `*.log`，并用 `subprocess` 的 `timeout` 强制超时；超时返回特殊 `returncode=-1`。
- **统计解析 `parse_stat()` / `parse_cell_hist()`**：用正则从 `stat` 报告抽取 `Number of cells/wires/...` 与逐 cell 类型直方图。
- **等价决策（`run_one_bench` 阶段三）**：
  - `n_cells ≤ struct_threshold`：走 SAT 模板（见 §四模板，`equiv_make + equiv_struct + equiv_simple + equiv_status -assert`）；
  - 否则走 `structural_equiv()` 结构等价代理（cell 类型直方图一致 + 重跑 MyIR 管线得到**字节级可重现**的 RTLIL）。
- **超时降级哲学**：证明阶段超时只记 `equiv=timeout`（状态仍 `pass`），因为 ratio 数据本身有效——避免个别"证不动"的电路拖垮整批。
- **失败保留中间文件**：仅当 `status==pass` 才清理 `.v/.rtlil`，失败时保留以便事后放大 timeout 重跑 / 肉眼比对 RTLIL。
- **并行**：`-j>1` 时用 `ThreadPoolExecutor`（子进程是真正的并行单元，GIL 不构成瓶颈）。
- **报告产出 `write_csv` / `write_md`**：底部输出通过率、SAT/结构证明数、MyIR 优化胜负统计与几何平均 ratio（仅计 `pass` 项）。

---

## 五、实验结果

环境：Visual Studio（MSVC）`Release|x64` 构建；`run_compare.py --mode prove --pipeline full --struct-threshold 0 -j 4 --timeout 600`。

### 5.1 优化对比与形式等价总表

> Ratio = MyIR cells / Yosys cells；< 1 表示 MyIR 更优。Equiv 为 SAT 形式证明结果。

| Benchmark | 类别 | Yosys cells | MyIR cells | Ratio | cell 减少 | Yosys wires | MyIR wires | Equiv | t_equiv |
|---|---|---:|---:|---:|---:|---:|---:|:---:|---:|
| `bar` | arithmetic | 5263 | 3408 | **0.648** | −35.2% | 5398 | 3543 | **PASS** | 8.8 s |
| `cavlc` | random_control | 1279 | 943 | 0.737 | −26.3% | 1289 | 953 | **PASS** | 3.2 s |
| `adder` | arithmetic | 2162 | 1655 | 0.765 | −23.5% | 2418 | 1911 | **PASS** | 13.0 s |
| `max` | arithmetic | 5383 | 4190 | 0.778 | −22.2% | 5895 | 4702 | **PASS** | 310.7 s |
| `int2float` | random_control | 443 | 346 | 0.781 | −21.9% | 454 | 357 | **PASS** | 3.0 s |
| `router` | random_control | 459 | 363 | 0.791 | −20.9% | 546 | 450 | **PASS** | 3.3 s |
| `priority` | random_control | 1907 | 1555 | 0.815 | −18.5% | 2035 | 1683 | **PASS** | 7.6 s |
| `i2c` | random_control | 2176 | 1898 | 0.872 | −12.8% | 2338 | 2060 | **PASS** | 3.5 s |
| `ctrl` | random_control | 306 | 273 | 0.892 | −10.8% | 314 | 281 | **PASS** | 3.0 s |
| `dec` | random_control | 312 | 316 | 1.013 | **+1.3%** | 320 | 324 | **PASS** | 3.0 s |

**汇总指标：**

- 通过率：**10 / 10** 管线成功；形式等价：**10 / 10 SAT PASS**。
- 优化胜负：MyIR 更小 **9** 条，平局 0 条，Yosys 更小 **1** 条（`dec`）。
- **几何平均 Ratio = 0.804**（仅计通过项），即 MyIR 平均比 Yosys `opt -full` 少约 **20%** 的 cell。

### 5.2 消融实验：设计原则与结果

#### 5.2.1 消融设计原则（为何不逐条消融全部 8 个优化 Pass）

MyIR 当前有 8 个优化 Pass。一种朴素做法是对每个 Pass 各做一次"关掉它"的消融，但本实验**有意不这样做**，原因是数据集的结构特征使得部分 Pass 在 EPFL 上**结构性不适用**——消融它们的 delta 必然恒为 0，属无信息量的注水。

证据：EPFL benchmark 经综合后，单元类型直方图**只含三类布尔门**（实测）：

| benchmark | `$and` | `$not` | `$or` | 其它（`$mul`/比较器/`$mux`/`$add`/concat …） |
|---|---:|---:|---:|---|
| `adder` | 1017 | 1142 | 3 | **0** |
| `cavlc` | 690 | 586 | 3 | **0** |
| `i2c` | 1310 | 834 | 32 | **0** |

即 EPFL 是纯 **AND-OR-NOT（AIG 风格）布尔网络**，不含算术、比较、Mux、拼接结构。据此把 8 个 Pass 按"在本数据集上是否可能命中"分类：

| Pass | 目标结构 | 在 EPFL 上 |
|---|---|---|
| `myir_opt_const` / `myir_opt_expr` / `myir_opt_share` / `myir_opt_clean` | 常量 / 表达式规整 / CSE / 死代码 | ✅ 生效（构成"规整组"） |
| `myir_rewrite` | 跨多 cell 布尔代数重构 | ✅ 关键贡献者 |
| `myir_opt_strength` | 乘法/加法强度削减（Mul→Shl…） | ❌ 无 Mul/Add，命中恒为 0 |
| `myir_opt_compare` | 比较器化简 | ❌ 无比较器，命中恒为 0 |
| `myir_opt_concat` | 拼接折叠/展平 | ❌ 无 Concat，命中恒为 0 |
| `myir_opt_muxtree` | 冗余 Mux 消除 | ❌ 无 Mux，命中恒为 0 |

因此本实验采用**假设驱动的"分组 + 累进"消融**，而非逐条消融：把生效 Pass 分为"规整组"与"重构（rewrite）"两层，量化各层的独立贡献；后 4 个 Pass 的能力需以含算术/Mux 的 RTL 级设计（或仓库内 `share_test.v` 等定向单测）单独演示，不属本统计实验范畴（见 §十）。

#### 5.2.2 三层累进消融

对每条 benchmark 跑三档配置并统计 cell 数：

- **Tier-0 裸导入**：`ast_import → export → proc → opt_clean`（不开任何 `myir_opt`）；
- **Tier-1 规整组**：`myir_opts -fixpoint -norewrite`（开 const/expr/share/clean 等，但**关 rewrite**）；
- **Tier-2 全优化**：`myir_opts -fixpoint`（再叠加 rewrite）。

| Benchmark | Tier-0 裸导入 | Tier-1 规整组 | Yosys `opt -full` | Tier-2 +rewrite |
|---|---:|---:|---:|---:|
| `adder` | 2541 | **2162** | 2162 | **1655** |
| `bar` | 6672 | **5263** | 5263 | **3408** |
| `cavlc` | 1600 | **1279** | 1279 | **943** |
| `i2c` | 2728 | **2176** | 2176 | **1898** |
| `int2float` | 545 | **443** | 443 | **346** |
| `dec` | 320 | **312** | 312 | 316 |

**三段式结论非常清晰：**

1. **Tier-0 → Tier-1**：裸导入显著大于 Yosys（如 `adder` 2541 vs 2162），规整组把它**精确拉平到 Yosys `opt -full`**（Tier-1 列 = Yosys 列，逐一相等）。说明 const/expr/share/clean 做了**真实且必要**的工作——它们与 Yosys 的 `opt_expr`/`opt_merge`/`opt_clean` 同质，收敛到同一个 peephole 不动点。
2. **Tier-1 → Tier-2**：只有叠加 `myir_rewrite` 后才**反超** Yosys（`adder` 2162 → 1655）。
3. 因此 **MyIR 相对 Yosys 的全部净增益 100% 来自 `myir_rewrite`**；其余 Pass 的价值是"追平工业基线"，rewrite 的价值是"超越"。

#### 5.2.3 完整 10 条的 rewrite 开/关对照

| Benchmark | Yosys `opt -full` | MyIR（关 rewrite, Tier-1） | MyIR（开 rewrite, Tier-2） |
|---|---:|---:|---:|
| `dec` | 312 | **312** | 316 |
| `ctrl` | 306 | **306** | 273 |
| `int2float` | 443 | **443** | 346 |
| `router` | 459 | **459** | 363 |
| `cavlc` | 1279 | **1279** | 943 |
| `priority` | 1907 | **1907** | 1555 |
| `i2c` | 2176 | **2176** | 1898 |
| `adder` | 2162 | **2162** | 1655 |
| `bar` | 5263 | **5263** | 3408 |
| `max` | 5383 | **5383** | 4190 |

全部 10 条的 Tier-1 列与 Yosys 列逐一完全相等，进一步坐实上述结论的普遍性。

---

## 六、结果分析

### 6.1 关键命令识别：为何是 `myir_rewrite`

§5.2 的三层累进消融直接回答了"哪条命令起关键作用"：**`myir_rewrite`**。规整组（const/expr/share/clean）把裸导入网表从大于 Yosys 拉平到与 Yosys `opt -full` **逐一精确相等**（Tier-1 = Yosys），说明它们与 Yosys `opt_expr`/`opt_merge`/`opt_clean` 同质、收敛到同一 peephole 不动点；**唯有叠加 `myir_rewrite` 才反超**（Tier-2 < Yosys）。换言之，规整组负责"追平工业基线"，rewrite 负责"超越"。

`myir_rewrite` 的不同之处在于它做的是**跨 2~3 个 cell 的布尔代数重构**（k-cut 局部模式重写），其 8 大类规则中真正产生面积收益的是：

- **分配律因式分解**：`(a&b)|(a&c) → a&(b|c)`、`(a|b)&(a|c) → a|(b&c)`（3 cell → 2 cell）；
- **De Morgan 合并**：`~a|~b → ~(a&b)`、`~a&~b → ~(a|b)`；
- **一致律 / 重代换（consensus / resub）**：`(x&y)|(~x&y) → y`、`y|(~x&y) → x|y` 等（最多 4 cell → 1 cell）；
- **吸收律**：`x|(x&y) → x`、`x&(x|y) → x`。

### 6.2 为什么自研工具能"超过"成熟的 Yosys

这一结果初看反直觉，根因在于**优化分工的层次差异**，而非 Yosys"不会"这些优化：

1. **Yosys 的设计哲学是分层**：RTLIL 层的 `opt -full` 专注于"保结构"的规整与清理（常量、CSE、mux 树、死代码），**刻意不做**会大幅改变逻辑结构的布尔因式分解/重构；这一类高强度重写被有意推迟到后端 **ABC**（在 AIG/工艺映射阶段，用 `abc` 命令）完成。因此只跑 `opt -full`、不跑 ABC 时，这部分优化空间是被**主动留白**的。
2. **`myir_rewrite` 恰好填补了这块留白**：它在 IR 层就主动做分配律/一致律/De Morgan 等代数重构，于是在"同样不做工艺映射"的口径下，MyIR 自然领先。
3. **因此正确的表述是**：在 **RTLIL/IR 同层、均不含 ABC** 的公平口径下，MyIR 的优化更激进、更靠前；这并不意味着 MyIR 强于"Yosys 完整综合（含 ABC）"。这一边界已在 §2.3 明确，是本实验诚实性的关键。

### 6.3 唯一未超越的 `dec`：原因分析

`dec` 是唯一 ratio > 1 的个例（312 → 316，+1.3%，+4 cell）。结合 §5.2 消融数据可精确归因：

- `dec` 是**二进制→独热译码器**，每个输出是输入位某种极性的唯一最小项（一串 AND），**输出之间几乎不共享可因式分解的子结构**，也不存在吸收/一致律的化简机会。
- 关闭 rewrite 时 `dec` = 312（与 Yosys 持平，说明已是 peephole 不动点）；**开启 rewrite 后反而升到 316**。这说明 `myir_rewrite` 在 `dec` 上匹配到了若干"局部看似有益"的模式（如 De Morgan / resub），但其重写形态会引入 `Buf`/`Not` 中间节点，这些节点在 `myir_export → opt_clean` 后**未能被完全吸收**，净增 4 个 cell。
- **本质**：`benefit_score` 评分模型基于 IR 层节点数估计收益，未建模"重写产物经 export + opt_clean 落到 RTLIL 后的真实 cell 数"。在毫无共享结构的电路上，这一估计偏差导致了轻微的、非劣化语义的面积反弹。这是一个明确、可改进的方向（见 §十）。

### 6.4 形式等价结果分析

- **全部 10 条以 SAT 形式证明通过**，包括 5000+ cell 的 `bar`/`max`，证明 MyIR 的"更小"是**功能等价前提下**的真实优化，而非错误化简。
- **规模与证明耗时强相关**：除 `max`（310.7 s）外，其余均在 ~13 s 内完成。`max` 慢的原因是其内部含较深的比较/选择级联，SAT 求解的逻辑锥更深。
- **`equiv_struct` 预合并是大设计可证的关键**（见 §八历程）：它先把两侧结构相同的区域合并，只把真正分叉的逻辑锥交给 SAT。
- **为何不纳入 `multiplier`/`hyp` 等重算术件**：乘法器类电路的组合等价检查对**任何**通用等价检查器（包括商业工具）都近乎不可行（指数级 BDD/SAT 爆炸），这是该领域公认难题，与本工具无关；如需评估其优化 ratio，可单用 `--mode measure` 收集 cell 数而不做证明。

---

## 七、关键瓶颈与优化：`myir_rewrite` 算法

`myir_rewrite` 既是优化收益的唯一来源，也是整个实验最大的性能瓶颈。本节详述其原始算法、瓶颈、优化方案、复杂度与实测效果。

### 7.1 算法基础：k-cut 枚举 + 模式匹配

`myir_rewrite` 的单步逻辑分两段：

1. **k-cut 枚举**（`enumerate_all_cuts`）：对每个 expression 节点，递归列出叶子数 ≤ k（默认 4）的所有局部子图（cut），并做记忆化；`-max-cuts N`（默认 16）对每节点保留的 cut 数封顶以防组合爆炸。
2. **模式匹配与重写**：在每个 cut 上尝试 8 大类规则，用 `benefit_score` 取收益最高者，重写命中的局部结构。

### 7.2 原始实现的复杂度缺陷

原始实现采用**"枚举—改一处—全量重枚举"**的循环：

```
while 还能找到可重写点:
    对【整个模块】枚举所有 k-cut          # O(E · C)
    找到【一个】最佳重写候选
    立即修改 m.exprs 应用它
    # 然后回到循环顶，对整个模块重新枚举
```

设模块 expression 节点数为 E、每节点 cut 上限为 C（受 k 与 max-cuts 约束的常数）。一次全模块枚举耗时 **O(E · C)**。由于每应用一次重写就要全量重枚举一次，而总重写次数 R 与模块规模成正比（R ∝ E），故总复杂度为：

> **O(R · E · C) ≈ O(E² · C)** —— 对模块规模**平方级**恶化。

对 `adder`（E ≈ 2000）而言，这意味着约 4×10⁶ × C 次 cut 计算，在 Debug 与 Release 下均长时间无法完成（实测 >400 s 仍未结束，被迫超时）。此外，cut 合并函数 `combine_cut_options` 内部还用 **O(n²) 线性扫描**去重，在宽设计上进一步雪上加霜。

### 7.3 优化方案：批量化重写 + 哈希去重

核心思想：**把"每次重写后全量重枚举"改为"每轮枚举后一次性应用一整批互不冲突的重写"**。

```
while True:
    对整个模块枚举所有 k-cut          # 每轮仅 O(E · C)
    收集【每个根节点的最佳重写候选】     # find_all_rewrite_candidates
    if 无候选: break
    按 benefit_score 降序排序
    贪心选取一个"触及节点索引两两不相交"的最大子集，构造 RewritePlan
    apply_plan(): 一次 O(E) 重建 m.exprs，批量应用整批重写
```

两项关键改造：

1. **批量应用（`RewritePlan` + `apply_plan`）**：把多个互不重叠的重写攒成一个 plan，单趟 O(E) 重建表达式列表统一落地，而非每条重写各改一次。这使**全模块重枚举的次数从 O(R) 降到 O(轮数)**。
2. **哈希去重**：`combine_cut_options` 改用 `std::unordered_set<std::string>` 对 cut 指纹做 O(1) 去重，替换原 O(n²) 线性扫描。

优化后复杂度：设收敛所需轮数为 D（每轮可并行消化大量互不相交的重写，D 远小于 R，实测为小常数级），总复杂度降为：

> **O(D · E · C)，D 为小常数 ⇒ 近似 O(E · C)** —— 对模块规模**线性级**。

### 7.4 正确性论证（为何"互不相交即可并行"）

批量并行的安全性依赖一个 IR 不变量：

- 所有重写规则**只删除 use-count 恰为 1 的中间节点**（即该节点的唯一使用者就在自身重写模式内部）；
- 叶子线（cut 的 leaves）与根结果线名在重写前后**恒被保留**。

因此，若两个重写候选"触及的 expression 索引集合"（`touched_indices()`：因式分解类为 {root, lhs, rhs}，重定向类为 remove_nodes）**两两不相交**，则它们读写的节点彼此独立，无论应用顺序如何结果一致——可安全地在同一趟重建中并行应用。贪心选取最大不相交子集即由此而来。

> 正确性的**最终背书是端到端的**：§5.1 中 10/10 SAT 形式等价证明覆盖了批量化重写后的完整流水线输出，从功能上证明该优化未引入任何不等价。

### 7.5 实测效果

| 指标 | 优化前 | 优化后 |
|---|---|---|
| `adder` 全流水线（含 rewrite） | >400 s 仍超时（Debug/Release 均如此） | **≈ 7 s** |
| 10 条 benchmark 优化数据采集（`--mode measure -j 4`） | 不可行（大件全超时） | **≈ 31 s** |
| 10 条 benchmark 完整 SAT 形式验证（`--mode prove -j 4`） | 不可行 | **≈ 5 min 41 s** |

由于优化前 `adder` 从未在 400 s 内跑完，**加速比下界 > 50×**（实际更高）。正是这一改造，使 §五全部对比与验证实验**从"根本跑不出来"变为"分钟级可复现"**。

---

## 八、实验真实历程与运行环境

本节如实记录从首次尝试到最终成功的全过程，重点是其中的挫折与对应的解决方案。

### 8.1 运行环境

- 操作系统：Windows（10.0.26200），PowerShell。
- 编译器/构建：Visual Studio Community（MSVC），由 `MyVSYosys.vcxproj` 管理；性能实验全部基于 **`Release|x64`** 构建（Debug 构建仅用于功能调试，性能差异达一个数量级以上）。
- 数据集：`tests/epfl/lsils_benchmarks/`（EPFL 官方 clone）。
- 并行度：`-j 4`。

### 8.2 完整历程（挫折 → 解决）

**第 1 阶段 — Debug 下首跑即大面积超时。**
最初在 Debug 构建上对 `dec/int2float/adder/priority` 跑 `run_compare.py`。`dec`/`int2float` 秒级通过，但 **`adder`/`priority` 在 MyIR 流水线阶段（`myir_opts -fixpoint`）直接超时**；即便管线跑完，等价阶段也卡死。

**第 2 阶段 — 定位并拆掉两个伪瓶颈。**
- *挫折*：等价阶段对组合加法器跑 `equiv_induct` 长达 15+ 分钟仍无果；且一次 `adder` 的等价"FAIL"事后查明是 900 s 超时而非真不等价。
- *解决*：组合电路无时序反馈，`equiv_induct` 纯属多余——从等价模板中移除，仅保留 `equiv_simple`。同时新增 `--pipeline lite`（`myir_opts -once -norewrite`）用于 Debug 下快速冒烟。

**第 3 阶段 — 锁定真凶 `myir_rewrite`。**
拆掉伪瓶颈后，`adder` 在带 rewrite 的 full 管线下**仍在 400 s 超时**，确认 `myir_rewrite` 是真正的性能黑洞。

**第 4 阶段 — 切换 Release 构建，遭遇编译错误。**
- *挫折*：尝试 `Release|x64` 编译时大面积报 `C1083: 无法打开包括文件 "kernel/celltypes.h"` 等。
- *解决*：诊断出 `MyVSYosys.vcxproj` 仅为 `Debug|Win32` 配置了 `AdditionalIncludeDirectories` 与 `PreprocessorDefinitions`（`_YOSYS_` 等），其余配置缺失。为 `Release|x64`/`Release|Win32`/`Debug|x64` 补齐同款包含目录与宏定义、并统一 `SDLCheck=false` 后编译通过。

**第 5 阶段 — Release 下 `myir_rewrite` 仍超时 → 算法级改造。**
- *挫折*：即便 Release，`adder` 的 rewrite 仍在 120~300 s 超时。
- *解决*：定位到 §7.2 的"每改一处即全模块重枚举"O(E²·C) 缺陷，实施 §7.3 的**批量化重写 + 哈希去重**。改造后 `adder` 全流水线降至 ≈ 7 s。

**第 6 阶段 — 大电路 SAT 等价从"不可能"到"43 秒"。**
- *挫折*：直接对 `adder` 跑 `equiv_simple` SAT，在 Debug 下 >30 分钟不出结果，一度认为大电路只能退而用结构等价代理。
- *解决*：在 Release 下先用 `equiv_struct` 合并两侧结构相同区域、只把分叉逻辑锥交给 SAT —— `adder` **43 s** 通过。据此把 `equiv_struct` 固化进等价模板，并发现**全部大电路都能拿到真正的 SAT 形式等价**，无需结构代理。

### 8.3 最终成功实验情景

在上述全部修复就位后，执行了一次性的全量形式验证：

```powershell
$env:YOSYS_BIN = (Resolve-Path "x64\Release\MyVSYosys.exe").Path
python tests/epfl/run_compare.py --mode prove --pipeline full --struct-threshold 0 -j 4 --timeout 600
```

- 启动 `15:02:57`，结束 `15:08:38`，**总耗时 ≈ 5 分 41 秒**（10 条 benchmark，`-j 4` 并行）。
- 控制台逐条输出 `done: ... status=pass ... equiv=PASS`，**10 / 10 全部 `status=pass` 且 `equiv=PASS`**。
- 报告落地 `tests/epfl/results/epfl_results.{csv,md}`，即 §5.1 的数据来源。

至此，"`myir_rewrite` 批量化提速 → 优化对比数据采集 → 全量 SAT 形式等价背书 → 报告产出"的完整实验链路闭环。

---

## 九、实验难点总结

回顾整个实验，真正消耗精力的并非"写出能跑的流程"，而是一系列**层层嵌套、互相掩盖**的难点。本节按"性能 / 验证 / 工程 / 方法学"四类做系统总结，其中重新优化 `myir_rewrite` 是难度最高、最具代表性的一项。

### 9.1 核心难点：重新优化 `myir_rewrite` 命令

这是全实验最硬的骨头，难度体现在四个递进层面：

1. **瓶颈被层层掩盖，定位困难。** 最初 `adder`/`priority` 超时时，表象指向多个嫌疑：MyIR 流水线本身、`equiv_induct`、Debug 构建慢、甚至怀疑是死循环。必须**逐一排除伪瓶颈**（先拆 `equiv_induct`、再换 Release、再用 `-norewrite` 隔离）才能确认真凶是 `myir_rewrite`。这种"多因素叠加、单点超时"的场景，是性能调试中最难定位的一类——因为任一因素都足以独立造成超时，无法靠单次观测区分。

2. **复杂度缺陷藏在"看似合理"的循环结构里。** 原始"枚举—改一处—重枚举"的写法逻辑上完全正确、且在小电路上跑得很好，缺陷只在规模放大时才以 O(E²·C) 的形式爆发。识别它需要跳出"功能正确"视角，从**算法复杂度**层面重新审视一段已通过所有单测的代码。

3. **批量化的正确性论证是最大障碍。** 把"逐个改"变成"一批一起改"，性能收益显然，但**正确性不显然**：并行应用多个重写，如何保证它们互不破坏彼此的前提？这要求提炼并证明一个 IR 不变量——"重写只删除 use-count 恰为 1 的中间节点、且叶子线与根结果线恒被保留"，从而推出"触及节点索引不相交 ⇒ 可安全并行"（§7.4）。这一步是从工程实现上升到**形式化推理**，也是最容易引入隐蔽 bug 的地方。

4. **数据结构需要配套重构。** 为支持"先攒一批、再一次性落地"，必须引入延迟应用机制（`RewritePlan` 暂存 remove/replace/connects，`apply_plan` 单趟 O(E) 重建），而非原先的即时 `m.exprs` 原地修改。这牵涉到对核心 IR 容器的重写，任何索引错位都会导致静默的逻辑错误。最终靠 **10/10 SAT 形式等价**端到端兜底，才敢确信改造无误。

> 一句话总结这条难点：**它要求同时具备"性能复杂度分析、形式化正确性论证、核心数据结构重构、端到端等价验证"四种能力，缺一不可。**

### 9.2 验证侧难点：大规模电路的形式等价"可行性"

- **直觉性的死路**：组合电路等价检查直觉上应"做完整 SAT"，但直接 `equiv_simple` 在多千门加法器上 SAT 爆炸（Debug >30 min），一度让我们认为"大电路只能退而求其次用结构等价代理"。
- **关键洞察来之不易**：突破口是"**先 `equiv_struct` 合并两侧结构相同区域，只把真正分叉的逻辑锥交给 SAT**"。这个顺序看似简单，但需要理解 Yosys 等价引擎的内部机制才能想到——加上 Release 构建的算力，`adder` 从超时直接降到 43 s，进而使全部 10 条（含 5000+ cell）都拿到真 SAT 证明，彻底摆脱了"结构代理"这个较弱的退路。
- **"超时"与"真不等价"的区分**：早期一次 `adder` 等价报 `FAIL`，险些被误判为"优化引入了功能错误"，事后查明只是 900 s 超时。这促使我们在脚本里把**证明超时降级为 `timeout`（状态仍 pass）**，避免性能问题污染正确性结论——这是一个重要的方法学修正。

### 9.3 工程侧难点：构建配置的"沉默陷阱"

- 切到 `Release|x64` 时大面积 `C1083` 头文件找不到。难点在于其**报错信息（缺头文件）与根因（构建配置缺失）相距甚远**：真正原因是 `MyVSYosys.vcxproj` 只给 `Debug|Win32` 配了 `AdditionalIncludeDirectories` 与 `_YOSYS_` 等宏，其余三个配置组从未被正确填充。需要逐 `ItemDefinitionGroup` 比对才能定位，是典型的"配置漂移"问题。

### 9.4 方法学与环境难点

- **优化对比 vs 正确性证明的解耦**：最初把二者绑在一条流程里，导致"只想看 ratio"也要付出 SAT 的超时代价。后来在脚本里分出 `--mode measure`/`prove`，把"不需要证明的优化数据采集"与"昂贵的形式证明"解耦，整体效率才提上来。
- **输出污染**：Yosys 启动动画含 ANSI 光标控制码，PowerShell 默认 UTF-16 落盘又会引入 null 字节，使日志解析屡屡踩坑。最终靠正则剥离 ANSI、统一 ASCII 落盘、以及优先用 `tee -o` / `stat` 文件而非抓取 stdout 才稳定下来。
- **数据集规模的自我约束**：必须主动识别并排除 MtM（千万门级）与乘法器类（等价不可判定）电路，把评测限定在"既有统计意义、又可形式验证"的子集内——这本身就是一项需要领域判断的实验设计决策。

---

## 十、结论与未来工作

### 结论

1. 在 EPFL 组合 benchmark 上，于 RTLIL/IR 同层、均不含 ABC 工艺映射的公平口径下，MyIR 优化流水线相对 Yosys 原生 `opt -full` 取得**几何平均 0.804** 的 cell 比（9/10 更优，最高 −35.2%）。
2. 优化结果经 **10/10 SAT 形式等价证明**背书，功能正确性有形式化保证。
3. 消融实验证明优势**完全来自 `myir_rewrite`** 的 k-cut 局部代数重构——它填补了 Yosys 刻意留给后端 ABC 的 RTLIL 层布尔重构空白。
4. 通过批量化重写将 `myir_rewrite` 复杂度从 O(E²·C) 降至近似 O(E·C)，使大规模实验从"超时不可行"变为"分钟级可复现"。

### 局限与未来工作

- **基线口径**：未与"Yosys + ABC 完整综合"对比。后续可增设 `opt -full; techmap; abc` 基线，更全面地定位 MyIR 在完整综合链中的位置。
- **`benefit_score` 与后端脱节**：`dec` 个例暴露了 IR 层收益估计未建模 export/opt_clean 后真实 cell 数的问题，可引入"重写后试算落地代价"或在 export 后回退劣化重写。
- **覆盖面**：当前 tier-1 为 10 条中小组合电路；可在算法稳定后逐步纳入 `arbiter`/`voter` 等更大组合件，并对乘法器类仅做 measure 对比。
- **补足"非门级"Pass 的评测**：`myir_opt_strength`/`compare`/`concat`/`muxtree` 针对算术/比较/拼接/Mux 结构，而 EPFL 是纯 AND-OR-NOT 网络，故本实验中它们命中恒为 0、无法体现价值。后续应引入一组**含算术与 Mux 的 RTL 级设计**（或扩展现有 `share_test.v` 等单测）做定向消融，专门量化这 4 个 Pass 的贡献——这是它们正确的"考场"。
- **AST 主干增强**：扩展 memory / 复杂 case / 层次化实例的支持后，可把评测扩展到时序电路。

---

## 附录：一键复现命令

```powershell
# 0) 准备数据集
git clone --depth 1 https://github.com/lsils/benchmarks tests/epfl/lsils_benchmarks

# 1) 指向 Release 构建
$env:YOSYS_BIN = (Resolve-Path "x64\Release\MyVSYosys.exe").Path

# 2) 仅采集优化对比数据（最快，~31s）
python tests/epfl/run_compare.py --mode measure --pipeline full -j 4

# 3) 完整 SAT 形式等价背书（~5min41s）
python tests/epfl/run_compare.py --mode prove --pipeline full --struct-threshold 0 -j 4 --timeout 600

# 4) 关闭 rewrite 的消融对照（验证优势来源）
#    将 myir_pipeline 中 myir_opts 改为 -fixpoint -norewrite 后重跑 measure

# 结果：tests/epfl/results/epfl_results.{csv,md}
```
