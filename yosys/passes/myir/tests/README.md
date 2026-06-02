# MyIR Per-Pass Unit Tests

This folder holds tiny `.v` inputs and `.ys` driver scripts that
exercise individual MyIR passes.  For the project-level EPFL
benchmark comparison harness, see [`../../../../tests/epfl/README.md`](../../../../tests/epfl/README.md).

## How to run

From the repo root, with a built `MyVSYosys.exe`:

```powershell
# Single test
.\x64\Debug\MyVSYosys.exe -s yosys\passes\myir\tests\test_all_opts.ys

# Or, drop into interactive mode
.\x64\Debug\MyVSYosys.exe -Q
yosys> script yosys/passes/myir/tests/test_ast.ys
```

## Index

### Verilog inputs

| Input | Pass(es) it stresses |
|-------|----------------------|
| `carry.v` | RTLIL & AST baseline; expression chain |
| `share_test.v` | `myir_opt_share` (CSE) |
| `mux_ifelse.v` | `myir_ast_import` (`AST_CASE` -> if/else) |
| `mux_case.v` | `myir_ast_import` (case lowering) |
| `dff_test.v` | always-block + posedge sensitivity |
| `const_test.v` | `myir_opt_const` (constant propagation, chain prop) |
| `muxtree_test.v` | `myir_opt_muxtree` (path-condition pruning) |
| `expr_test.v` | `myir_opt_expr` (folding + algebra + mutual complement) |
| `rewrite_test.v` | `myir_rewrite` (k-cut + sub_self / or_comp / and_comp) |
| `escaped_id_test.v` | `norm_ident` (B1/B2) + `to_id` (A1) round-trip |
| `primitives_test.v` | `handle_primitive` (F2) for AST_PRIMITIVE |

### Yosys driver scripts

| Script | Purpose |
|--------|---------|
| `test_e2e.ys` | RTLIL route end-to-end |
| `test_ast.ys` | AST route end-to-end (project's primary path) |
| `test_share.ys` | `myir_opt_share` |
| `test_const.ys` | `myir_opt_const` |
| `test_muxtree.ys` | `myir_opt_muxtree` |
| `test_expr.ys` | `myir_opt_expr` |
| `test_rewrite.ys` | `myir_rewrite -stats` |
| `test_all_opts.ys` | `myir_opts` orchestrator vs manual chain |
| `test_escaped_id.ys` | Escaped-identifier round-trip + formal equiv |
| `test_primitives.ys` | AST_PRIMITIVE round-trip + formal equiv |

## Conventions

- Inputs should be **small** (< 30 lines) and focused on one feature.
- Driver scripts should always log a `===== ... PASS =====` line
  at the end so a grep across many runs surfaces failures quickly.
- For equivalence-style tests, use `equiv_status -assert` so any
  non-equivalence fails the pass (Yosys returns a non-zero exit).
- When adding a new test, also list it in this README's tables AND
  the project-level [`tests/README.md`](../../../../tests/README.md).
