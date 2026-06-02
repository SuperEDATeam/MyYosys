# Tests Directory Index

This directory holds **project-level** test infrastructure for
MyVSYosys.  Per-pass unit tests live alongside the corresponding
sources at `yosys/passes/myir/tests/` (see bottom of this file).

## Sub-folders

| Folder | Purpose | Entry point |
|--------|---------|-------------|
| [`epfl/`](epfl/README.md) | Public benchmark comparison harness (Yosys baseline vs MyIR pipeline + formal equiv) on the [EPFL combinational benchmarks](https://github.com/lsils/benchmarks) | `python tests/epfl/run_compare.py` |

## Quick reference — EPFL harness

| Asset | Description |
|-------|-------------|
| [`epfl/README.md`](epfl/README.md) | Full setup + usage + troubleshooting |
| [`epfl/benchmarks.txt`](epfl/benchmarks.txt) | Default benchmark selection, with MtM segregation |
| [`epfl/templates/`](epfl/templates/) | Yosys script templates (baseline / myir / equiv) |
| [`epfl/run_compare.py`](epfl/run_compare.py) | Python driver (subprocess + stat parsing + CSV/MD reports + `-j` parallel) |
| [`epfl/results/`](epfl/results/) | Reports & per-benchmark logs (generated) |

## Per-pass unit tests (inside `yosys/passes/myir/tests/`)

The unit tests are kept beside the passes so a fresh checkout
finds them next to the code they exercise.

### Verilog inputs

| File | Exercises |
|------|-----------|
| `carry.v` | Basic combinational logic (carry chain) |
| `share_test.v` | Shared sub-expression detection |
| `mux_ifelse.v` / `mux_case.v` | `if/else` / `case` -> `Statement` lowering |
| `const_test.v` | Constant propagation & folding |
| `muxtree_test.v` | Mux-tree dead branch elimination |
| `expr_test.v` | Expression rewriting / mutual-complement rules |
| `rewrite_test.v` | k-cut algebraic rewriting |
| `dff_test.v` | DFF + async reset (always block path) |
| `escaped_id_test.v` | EPFL-style escaped identifier round-trip (`\name[i]`) |
| `primitives_test.v` | `AST_PRIMITIVE` lowering (and / or / xor / nand / ...) |

### Yosys driver scripts

| Script | Drives |
|--------|--------|
| `test_e2e.ys` | Full RTLIL-route end-to-end |
| `test_ast.ys` | AST-route end-to-end (including RTLIL-vs-AST comparison) |
| `test_share.ys` | `myir_opt_share` regression |
| `test_const.ys` | `myir_opt_const` regression |
| `test_muxtree.ys` | `myir_opt_muxtree` regression |
| `test_expr.ys` | `myir_opt_expr` regression |
| `test_rewrite.ys` | `myir_rewrite` regression (+ stats) |
| `test_all_opts.ys` | `myir_opts -fixpoint -stats -k 4` vs manual chain |
| `test_escaped_id.ys` | escaped-identifier round-trip + formal equiv |
| `test_primitives.ys` | `AST_PRIMITIVE` round-trip + formal equiv |

Run any of them inside the built `MyVSYosys` binary, e.g.

```powershell
.\x64\Debug\MyVSYosys.exe -s yosys\passes\myir\tests\test_all_opts.ys
```
