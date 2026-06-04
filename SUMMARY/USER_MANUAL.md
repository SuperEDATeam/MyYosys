# MyIR 用户使用手册

> 适用对象：MyIR 自研中间表示（Intermediate Representation）及其优化/验证命令集
> 宿主平台：基于 Yosys 的 Visual Studio 移植版（`MyVSYosys.exe`）
> 配套脚本：`tests/epfl/run_compare.py`（EPFL benchmark 优化对比与等价验证驱动器）
> 配套文档：`SUMMARY/PROJECT_REPORT.md`（设计报告）、`SUMMARY/EXPERIMENT_REPORT.md`（实验报告）

---

## 目录

1. [概述](#1-概述)
2. [快速开始](#2-快速开始)
3. [核心概念](#3-核心概念)
4. [命令总览](#4-命令总览)
5. [导入 / 导出 / 可视化命令](#5-导入--导出--可视化命令)
6. [优化命令](#6-优化命令)
7. [验证命令](#7-验证命令)
8. [run_compare.py 脚本](#8-run_comparepy-脚本)
9. [典型工作流](#9-典型工作流)
10. [附录：命令与参数速查表](#10-附录命令与参数速查表)

---

## 1. 概述

**MyIR** 是构建在 Yosys 之上的一套自定义中间表示与优化器。它以 Yosys 的 pass 机制注册为一系列 `myir_*` 命令，可在 Yosys 交互式 shell 或脚本（`.ys`）中像任何内建命令一样调用。

MyIR 的整体数据流是：

```
Verilog 源码
   │  read_verilog
   ▼
Verilog AST  ──────────────►  RTLIL（Yosys 原生网表）
   │  myir_ast_import (主路径)        │  myir_import (对比路径)
   ▼                                  ▼
            MyIR::Design（全局中间表示）
   │  myir_opts / myir_opt_* / myir_rewrite（优化）
   ▼
   │  myir_export
   ▼
RTLIL  ──►  write_verilog / write_rtlil / equiv_* / stat ...
```

- **主路径 `myir_ast_import`**：直接从 Verilog AST 构建 MyIR，保留表达式树结构与过程语句（if/else、case），是项目的主分支。
- **对比路径 `myir_import`**：从 Yosys 已综合的 RTLIL 构建 MyIR，主要用于与主路径做对照。

所有 MyIR 命令共享一个**全局** `MyIR::Design` 对象（见 [核心概念](#3-核心概念)）。

---

## 2. 快速开始

最常用的「AST 主路径 + 完整优化 + 导出」流程：

```tcl
read_verilog design.v          # 读入 Verilog，构建 AST
hierarchy -auto-top            # 确定顶层模块
myir_ast_import                # AST -> MyIR
myir_opts -fixpoint            # 运行完整优化流水线，迭代到不动点
myir_export                    # MyIR -> RTLIL
proc; opt_clean                # 收尾，清理
stat                           # 查看 cell/wire 统计
write_verilog out.v            # 写出优化后的 Verilog
```

把以上写进一个 `.ys` 文件后，可用命令行批处理执行：

```powershell
x64\Release\MyVSYosys.exe -s flow.ys
```

---

## 3. 核心概念

| 概念 | 说明 |
|---|---|
| **`MyIR::Design`（全局单例）** | 所有 MyIR 命令读写同一个进程内的全局 MyIR 设计。`myir_import` / `myir_ast_import` 会先 `clear()` 再重建它；其余命令在其上原地操作。 |
| **Module / Port / Expr / Assign / ProcBlock** | MyIR 模块由端口（Port）、表达式（Expr，带 `op` + 操作数 + 结果线宽）、连接（Assign，即 `connect`）以及过程块（ProcBlock，来自 always 块）构成。 |
| **常量命名** | 形如 `4'1010` 表示 4 位常量，MyIR 内部用字符串携带常量，避免单独的常量节点。 |
| **`OpType`** | MyIR 的算子枚举（And/Or/Not/Xor/Mux/Add/Mul/Eq/Lt/Concat/Reduce_* 等）。与 RTLIL `$cell` 通过 `optype_to_celltype()` / `celltype_to_optype()` 双向映射。 |
| **运行顺序约定** | 优化命令假设 MyIR 已加载；若全局设计为空会给出 warning 并跳过。导入命令前需先 `read_verilog`（AST 路径）或先有 RTLIL（RTLIL 路径，通常先 `proc`）。 |

---

## 4. 命令总览

MyIR 共提供 **18 条命令**，分三大类：

| 类别 | 命令 |
|---|---|
| **导入 / 导出 / 可视化** | `myir_import`、`myir_ast_import`、`myir_export`、`myir_dump`、`myir_show`、`myir_view` |
| **优化** | `myir_opts`（流水线驱动）、`myir_opt_const`、`myir_opt_expr`、`myir_opt_compare`、`myir_opt_strength`、`myir_opt_concat`、`myir_opt_muxtree`、`myir_rewrite`、`myir_opt_share`、`myir_opt_clean` |
| **验证** | `myir_reliable` |

> 提示：在 Yosys 内对任意命令输入 `help <命令名>` 可查看其内置帮助。

---

## 5. 导入 / 导出 / 可视化命令

### 5.1 `myir_ast_import` —— Verilog AST → MyIR（主路径）

- **含义/功能**：遍历 `read_verilog` 后挂在每个模块上的 Verilog AST，直接构建 MyIR。相比 `myir_import`，它保留了表达式树结构与过程构造（if/else、case、连续/过程赋值），是项目主分支。
- **实现原理**：递归地把 AST 节点翻译为 MyIR——`AST_WIRE` 建端口、`AST_ASSIGN` 经 `flatten_assign` 展平为 Expr 链、`AST_PRIMITIVE`（Verilog 原语门 and/or/not/...）经 `handle_primitive` 翻译、`AST_ALWAYS` 转为 ProcBlock。所有标识符经 `norm_ident` 归一化（去尾空格、补 `\` 前缀），保证转义标识符（如 `\name[i]`）正确往返。
- **使用前提**：先 `read_verilog`（且建议 `hierarchy -auto-top`）。结果存入全局 `MyIR::Design`，可用 `myir_dump` 查看。

| 参数 | 说明 |
|---|---|
| `-strict` | 把「遇到无法处理的 AST 节点类型，用 `1'0`/`?` 占位符兜底」升级为硬错误。默认（非 strict）会静默用占位符替换并计入末尾汇总；加 `-strict` 则在首次遇到时直接 `log_error` 中止。**在 EPFL 等需要保证不发生静默 IR 损坏的验证流程中务必加上。** |

**末尾汇总字段**（始终打印）：导入模块数、`expr fallback hits`（flatten_expr 未能处理的 AST 节点类型计数）、`stmt fallback hits`（extract_stmt 同上）。

```tcl
read_verilog design.v
hierarchy -auto-top
myir_ast_import -strict
```

### 5.2 `myir_import` —— RTLIL → MyIR（对比路径）

- **含义/功能**：把当前 RTLIL 设计转换为 MyIR，主要用于与 AST 主路径做对照。
- **实现原理**：遍历选中模块的 wire（建端口）、cell（经 `celltype_to_optype` 转 Expr，不支持的 cell 类型给 warning 并跳过）、connections（转 Assign）。
- **使用前提**：若设计含 always 块等过程构造，先运行 `proc`。
- **参数**：无（仅命令名本身）。

```tcl
read_verilog design.v
proc
myir_import
```

### 5.3 `myir_export` —— MyIR → RTLIL

- **含义/功能**：把当前 `MyIR::Design` 写回成 RTLIL 模块；同名 RTLIL 模块会被替换。导出后即可用 Yosys 全套后端（`write_verilog`/`write_rtlil`/`equiv_*`/`stat`）。
- **实现原理**：先建 wire/port（经 `to_id` 统一加 `\` 前缀），再把每个 Expr 通过 `optype_to_celltype` 建成 `$cell`。对比较类/归约类算子强制 `Y_WIDTH = 1`（`op_returns_1bit`）以匹配 RTLIL 语义。

| 参数 | 说明 |
|---|---|
| `-strict` | 把「静默丢弃」升级为硬错误。默认（best-effort）模式会跳过任何无法表达为 RTLIL cell 的东西但计入汇总；加 `-strict` 时，以下情况一律 `log_error` 中止：① 模块里残留 proc_block（说明 always/initial 未被完全下译）；② 任何 `OpType::Unknown` 的 Expr；③ 任何在 `optype_to_celltype()` 中无 `$cell` 映射的 Expr。 |

**末尾汇总字段**：导出模块数、丢弃的 proc_blocks 数、丢弃的 Unknown exprs 数、丢弃的 no-celltype 数。

```tcl
myir_export -strict
```

### 5.4 `myir_dump` —— 文本转储

- **含义/功能**：以人类可读的文本格式打印当前 MyIR 设计，便于调试。
- **参数**：

| 参数 | 说明 |
|---|---|
| `[filename]`（位置参数，可选） | 给出文件名则写入该文件；否则打印到 Yosys 日志（stdout）。 |

```tcl
myir_dump                 # 打印到屏幕
myir_dump design.myir     # 写入文件
```

### 5.5 `myir_show` —— 生成 DOT 图 / 图片

- **含义/功能**：为当前 MyIR 设计生成 Graphviz DOT 文件并渲染成图片。
- **实现原理**：先写 `.dot`，再按顺序查找 `dot` 可执行文件（① `<exe目录>/tools/graphviz/bin/dot` 及最多 3 层父目录；② 环境变量 `MYIR_DOT`；③ 系统 PATH），调用渲染。

| 参数 | 说明 |
|---|---|
| `-o <file>` | DOT 输出路径（默认 `myir_show.dot`）。 |
| `-fmt <format>` | 图片格式 `png\|pdf\|svg`（默认 `png`）。 |
| `-view` | 生成后用操作系统默认关联程序打开图片。 |
| `-viewer <cmd>` | 用自定义命令打开图片（优先级高于 `-view`）。 |
| `-no-image` | 只写 DOT，不调用 Graphviz。 |

```tcl
myir_show -o g.dot -fmt svg -view
myir_show -no-image          # 仅生成 .dot
```

### 5.6 `myir_view` —— 打开已生成的文件

- **含义/功能**：用系统默认关联程序（或自定义命令）打开一个**已存在**的图片/DOT 文件。与 `myir_show` 不同，它**不重新生成**，只负责打开。
- **参数**：

| 参数 | 说明 |
|---|---|
| `<file>`（位置参数，必填） | 要打开的文件路径。 |
| `-viewer <cmd>` | 用 `<cmd> "<file>"` 打开，替代系统默认查看器。 |

```tcl
myir_show                    # 仅生成文件
myir_view myir_show.png      # 稍后查看，不重新生成
```

---

## 6. 优化命令

### 6.1 `myir_opts` —— 优化流水线驱动器（推荐入口）

- **含义/功能**：一条命令跑完整套标准 MyIR 优化流水线，是日常使用的推荐入口。
- **流水线顺序**：
  ```
  myir_opt_const → myir_opt_expr → myir_opt_compare → myir_opt_strength
  → myir_opt_concat → myir_opt_const → myir_opt_muxtree → myir_opt_expr
  → myir_rewrite → myir_opt_share → myir_opt_clean
  ```
- **实现原理**：默认执行一轮；`-fixpoint` 时用「设计指纹」（模块的 expr/connect/wire 数哈希）判断是否还有变化，迭代直到无变化或满 10 轮。

| 参数 | 说明 |
|---|---|
| `-once` | 流水线只跑一轮（**默认**）。 |
| `-fixpoint` | 迭代最多 10 轮，某轮无 IR 变化时提前停止。 |
| `-k N` | 转发给 `myir_rewrite -k N`（k-cut 叶子数上限）。 |
| `-stats` | 转发给 `myir_rewrite -stats`（打印每条规则命中数）。 |
| `-noconst` | 跳过 `myir_opt_const`。 |
| `-noexpr` | 跳过 `myir_opt_expr`。 |
| `-nocompare` | 跳过 `myir_opt_compare`。 |
| `-nostrength` | 跳过 `myir_opt_strength`。 |
| `-noconcat` | 跳过 `myir_opt_concat`。 |
| `-nomuxtree` | 跳过 `myir_opt_muxtree`。 |
| `-norewrite` | 跳过 `myir_rewrite`（关掉最耗时、也是优化主力的代数重写层）。 |
| `-noshare` | 跳过 `myir_opt_share`。 |
| `-noclean` | 跳过 `myir_opt_clean`。 |

> 约束：`-k` 若给出必须 ≥ 2。

```tcl
myir_opts -fixpoint                  # 完整优化，迭代到不动点
myir_opts -once -norewrite           # 轻量流水线（跳过 k-cut 重写，最快）
myir_opts -fixpoint -k 6 -stats      # 用更大 k-cut，并打印规则命中统计
```

### 6.2 `myir_rewrite` —— k-cut 局部代数重写

- **含义/功能**：MyIR 的**优化主力**。对每个表达式枚举 k-cut，在 cut 上做基于模式的局部代数重写。MyIR 相对 Yosys 的 cell 数优势主要来自此命令。
- **实现原理**：每轮先枚举所有 k-cut，再按收益排序，**贪心选取一批「触及节点索引两两不相交」的重写一次性应用**（批量化），然后重新枚举、重复，直到无可获利的匹配。批量化把「全模块重枚举」的次数从 O(重写数) 降到 O(轮数)，使数千 cell 的设计也能秒级跑完。

| 参数 | 说明 |
|---|---|
| `-k N` | 一个 cut 允许的最大不同叶子数（**默认 4**，必须 ≥ 2）。 |
| `-max-cuts N` | 每个节点保留的 cut 数上限，超出按「叶子少优先、内部节点少次之」排序后截断；`0` 表示不限。**默认 16**。 |
| `-stats` | 在汇总日志中额外打印每条规则的命中数。 |

**支持的重写规则（节选）**：
```
(a & b) | (a & c)  -> a & (b | c)      ~a | ~b -> ~(a & b)
(a | b) & (a | c)  -> a | (b & c)      ~a & ~b -> ~(a | b)
(x & y) | (~x & y) -> y                x | (x & y) -> x
y | (~x & y)       -> x | y            x | x / x & x -> x
x ^ x -> 0    x ~^ x -> 1    !!x -> x  x&0 / x|0 / x&1 / x|1 / x^0
```

```tcl
myir_rewrite                 # 默认 k=4, max-cuts=16
myir_rewrite -k 6 -max-cuts 0 -stats
```

### 6.3 单步优化命令（均无可选参数）

下列命令只接受命令名本身，通常由 `myir_opts` 统一调度；单独调用便于调试某一类优化。

| 命令 | 含义/功能 | 实现要点 |
|---|---|---|
| `myir_opt_const` | **常量传播** | 透过 `connect` 记录与单操作数 Buf/Pos 表达式（即 `assign y = x` 式拷贝）传播常量。表达式折叠本身交给 `myir_opt_expr`。 |
| `myir_opt_expr` | **表达式化简 / 常量折叠** | 全常量折叠（28+ 算子，含 Lt/Le/Gt/Ge、移位、Reduce_And/Xor/Xnor）、部分折叠（x&0、x\|0、x*1）、门退化为线/反相器、互补律（x−x⇒0、x\|~x⇒−1、x&~x⇒0）、幂等律、简单 mux 化简。 |
| `myir_opt_compare` | **比较器化简** | 处理 `myir_opt_expr` 全常量折叠够不到的比较：自比较（x==x⇒1、x<x⇒0 等），以及仅对**无符号**操作数的 `x<0⇒0`、`x>=0⇒1`。结果化为 1-bit connect，利于 `myir_opt_muxtree` 接力。 |
| `myir_opt_strength` | **算术强度削减** | `x*2^k⇒x<<k`、`x+x⇒x<<1`、`0−x⇒−x`、`−(−x)⇒x`。保持位级语义，不引入新内部线。建议在 const/expr 之后运行。 |
| `myir_opt_concat` | **Concat 折叠/展平** | 全常量 Concat⇒单常量、单操作数 Concat⇒Buf、相邻常量合并、嵌套单用 Concat 展平。operand[0] 为最高位，与 Verilog `{a,b,c}` 一致。 |
| `myir_opt_muxtree` | **Mux 树死支消除** | 把级联 Mux 当作 mux 树分析，删除因祖先 mux 已约束 select 而永不可选的分支；并沿简单 Not/Buf/Pos select 传播路径条件，折叠 A/B 相同的 mux。 |
| `myir_opt_share` | **公共子表达式消除（CSE）** | 识别算子与操作数完全相同的表达式并合并，只保留一份计算，后续引用改用共享结果线。 |
| `myir_opt_clean` | **死代码消除（DCE）** | 删除结果线从未被任何表达式/连接/输出端口引用的表达式，并清理对应内部线。 |

```tcl
# 单独调试某一步
myir_opt_const
myir_opt_expr
myir_opt_muxtree
```

---

## 7. 验证命令

### 7.1 `myir_reliable` —— 逐模块等价可靠性检查

- **含义/功能**：对当前选中的每个模块，验证 `AST → MyIR → RTLIL` 转换（可选含优化）是否与原始 AST 综合结果在功能上等价，用于回归与定位失效点。
- **实现原理**：对每个模块构建两侧——
  - `gold` = 原始 AST → proc/memory/opt_clean → RTLIL
  - `gate` = 原始 AST → `myir_ast_import` →（可选优化）→ `myir_export` → proc/memory/opt_clean → RTLIL

  再用 `equiv_make` → `equiv_simple` → `equiv_induct` → `equiv_status -assert` 判等。每个模块经 7 个内部阶段（gold、ast_import、opts、export、gate_post、equiv_make、equiv_check）跟踪，失败可精确归因到某一阶段。

| 参数 | 说明 |
|---|---|
| `-with-opts` | 在 `myir_ast_import` 与 `myir_export` 之间插入 `myir_opts -fixpoint`，使检查覆盖完整优化流水线。默认不优化（只验证「翻译再翻回」）。 |
| `-stage <name>` | 单独验证某一个优化 pass（替代 `-with-opts`）。可选名：`ast`（不优化，等同默认）、`const`、`muxtree`、`expr`、`rewrite`、`share`、`clean`、`opts`（等同 `-with-opts`）。 |
| `-strict-export` | 把 `-strict` 转发给 `myir_export`，使任何静默丢弃升级为硬错误，并在报告中表现为 Export 阶段失败。 |
| `-recursive` | 每轮验证复制所有模块（`design -copy-from src *`），而非只复制被测模块。层次化（含子模块）设计需要；EPFL 扁平设计不需要。 |
| `-report <file>` | 写出逐模块 CSV 报告：`module,ok,fail_stage,note`。 |

**末尾汇总**：按阶段分类统计不可靠模块，从而区分「export 丢了东西」与「优化产生了不等价 RTLIL」。

```tcl
read_verilog design.v
hierarchy -auto-top
myir_reliable -with-opts -strict-export -report rel.csv
```

---

## 8. run_compare.py 脚本

### 8.1 用途

`tests/epfl/run_compare.py` 是一个**外部批量对比驱动器**（Python，独立于 Yosys 进程）。对每个 benchmark `.v` 文件，它独立起多个 Yosys 进程跑：

1. **Yosys 基线**：`read_verilog → proc → opt -full → clean → stat`
2. **MyIR 流水线**：`read_verilog → myir_ast_import → myir_opts → myir_export → opt_clean → stat`
3. **（prove 模式才做）等价验证**：对两侧输出做形式等价

最终在 `tests/epfl/results/` 下生成 `epfl_results.csv`（机器可读）与 `epfl_results.md`（人读表格 + 几何平均 ratio 摘要）。

### 8.2 两种运行模式（`--mode`）

| 模式 | 行为 | 速度 |
|---|---|---|
| `measure` | 只跑步骤 1、2，收集 cell 数比值（ratio）。**优化对比不需要等价证明**，因此不会付出 SAT 的代价。 | 快 |
| `prove` | 额外做等价证明：≤ `--struct-threshold` cell 用 SAT（`equiv_simple`）；超过则用更省的**结构等价**（cell 类型直方图一致 + RTLIL 字节可复现）。证明超时降级为 `equiv=timeout`（ratio 仍有效），不判 benchmark 失败。 | 慢 |
| `both` | measure + prove（**默认**）。 | 慢 |

### 8.3 全部命令行参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--yosys <path>` | 自动探测 | Yosys/MyVSYosys 可执行文件路径。 |
| `--epfl-root <dir>` | `tests/epfl/lsils_benchmarks` | EPFL benchmark 根目录。 |
| `--bench-list <file>` | `tests/epfl/benchmarks.txt` | benchmark 清单文件。 |
| `--filter <regex>` | 空 | 正则；只运行路径匹配的 benchmark。 |
| `--include-mtm` | 关 | 纳入超大的 MtM（百万门级）benchmark。 |
| `--mode {measure,prove,both}` | `both` | 运行模式，见 [8.2](#82-两种运行模式--mode)。 |
| `--no-equiv` | 关 | `--mode measure` 的别名，跳过等价证明。 |
| `--with-reliable` | 关 | （保留参数，当前脚本未实际调用 `myir_reliable`）。 |
| `--timeout <sec>` | `600` | 每个 Yosys 进程的超时（秒）。 |
| `-j, --jobs <N>` | `1` | 并行 benchmark 数。 |
| `--keep-intermediate` | 关 | 保留中间 `.v`/`.rtlil` 文件（默认仅在未完全 pass 时保留，便于复现/排查）。 |
| `--out-dir <dir>` | `tests/epfl/results` | 报告输出目录。 |
| `--pipeline {full,lite}` | `full` | MyIR 流水线变体：`full` 用 `myir_opts -fixpoint`；`lite` 用 `myir_opts -once -norewrite`（关掉 k-cut 重写，Debug 构建上对千门级设计快得多）。 |
| `--struct-threshold <N>` | `1000` | 当 `max(yosys_cells, myir_cells)` 超过该值时，跳过 SAT 等价、改用结构等价（记为 `PASS-struct`）。设 `0` 表示始终用 SAT。 |

### 8.4 报告字段含义

- `ratio = myir_cells / yosys_cells`：< 1 表示 MyIR 比 Yosys `opt -full` 后 cell 更少（更优）；> 1 表示 Yosys 更激进。
- `equiv` 取值：`PASS`（SAT 证明通过）、`PASS-struct`（结构等价通过）、`timeout`/`timeout-struct`（证明超时，ratio 仍有效）、`measure-only`（measure 模式未做证明）、`FAIL`/`FAIL-struct`（不等价/结构不匹配）。
- 摘要含通过数、SAT/结构证明数、MyIR 更小/打平/更大的 benchmark 计数、几何平均 ratio。

### 8.5 使用示例（Windows PowerShell）

```powershell
# 指定 Release 二进制
$env:YOSYS_BIN = (Resolve-Path "x64\Release\MyVSYosys.exe").Path

# 1) 最快拿优化对比数据（只数门、不做证明、4 路并行）
python tests/epfl/run_compare.py --mode measure --pipeline full -j 4

# 2) 只跑指定 benchmark，并做等价证明
python tests/epfl/run_compare.py --filter "dec|int2float|adder" --mode prove -j 3

# 3) 大设计自动用结构等价（阈值 1000），小设计用 SAT
python tests/epfl/run_compare.py --mode both --struct-threshold 1000 -j 4
```

---

## 9. 典型工作流

### 9.1 主路径：AST 导入 + 完整优化 + 导出

```tcl
read_verilog design.v
hierarchy -auto-top
myir_ast_import
myir_opts -fixpoint
myir_export
proc; opt_clean
stat
write_verilog optimized.v
```

### 9.2 对照路径：RTLIL 导入

```tcl
read_verilog design.v
proc
myir_import
myir_opts -fixpoint
myir_export
stat
```

### 9.3 调试单个优化 pass

```tcl
read_verilog design.v
hierarchy -auto-top
myir_ast_import
myir_dump before.myir       # 优化前快照
myir_rewrite -stats         # 只跑代数重写，看规则命中
myir_dump after.myir        # 优化后快照
```

### 9.4 可视化

```tcl
read_verilog design.v
hierarchy -auto-top
myir_ast_import
myir_opts -fixpoint
myir_show -fmt svg -view
```

### 9.5 等价可靠性回归

```tcl
read_verilog design.v
hierarchy -auto-top
myir_reliable -with-opts -strict-export -report rel.csv
```

---

## 10. 附录：命令与参数速查表

| 命令 | 参数 | 一句话 |
|---|---|---|
| `myir_ast_import` | `[-strict]` | Verilog AST → MyIR（主路径） |
| `myir_import` | （无） | RTLIL → MyIR（对比路径） |
| `myir_export` | `[-strict]` | MyIR → RTLIL |
| `myir_dump` | `[filename]` | 文本转储 |
| `myir_show` | `[-o <file>] [-fmt png\|pdf\|svg] [-view] [-viewer <cmd>] [-no-image]` | 生成 DOT/图片 |
| `myir_view` | `<file> [-viewer <cmd>]` | 打开已生成文件 |
| `myir_opts` | `[-once\|-fixpoint] [-k N] [-stats] [-noconst] [-noexpr] [-nocompare] [-nostrength] [-noconcat] [-nomuxtree] [-norewrite] [-noshare] [-noclean]` | 优化流水线驱动器 |
| `myir_rewrite` | `[-k N] [-max-cuts N] [-stats]` | k-cut 代数重写（优化主力） |
| `myir_opt_const` | （无） | 常量传播 |
| `myir_opt_expr` | （无） | 表达式化简/常量折叠 |
| `myir_opt_compare` | （无） | 比较器化简 |
| `myir_opt_strength` | （无） | 算术强度削减 |
| `myir_opt_concat` | （无） | Concat 折叠/展平 |
| `myir_opt_muxtree` | （无） | Mux 树死支消除 |
| `myir_opt_share` | （无） | 公共子表达式消除 |
| `myir_opt_clean` | （无） | 死代码消除 |
| `myir_reliable` | `[-with-opts] [-stage <name>] [-strict-export] [-recursive] [-report <file>]` | 逐模块等价可靠性检查 |

| `run_compare.py` 参数 | 默认 | 说明 |
|---|---|---|
| `--yosys` | 自动 | 可执行文件路径 |
| `--epfl-root` | `tests/epfl/lsils_benchmarks` | benchmark 根目录 |
| `--bench-list` | `tests/epfl/benchmarks.txt` | 清单文件 |
| `--filter` | 空 | benchmark 路径正则 |
| `--include-mtm` | 关 | 纳入 MtM 超大件 |
| `--mode` | `both` | `measure`/`prove`/`both` |
| `--no-equiv` | 关 | = `--mode measure` |
| `--with-reliable` | 关 | 保留（未启用） |
| `--timeout` | `600` | 每进程超时(秒) |
| `-j, --jobs` | `1` | 并行数 |
| `--keep-intermediate` | 关 | 保留中间文件 |
| `--out-dir` | `tests/epfl/results` | 报告目录 |
| `--pipeline` | `full` | `full`/`lite` |
| `--struct-threshold` | `1000` | 超此 cell 数改用结构等价 |
