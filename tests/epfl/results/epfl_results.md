# EPFL Benchmark Comparison Report

| Benchmark | Status | Yosys cells | MyIR cells | Ratio | Yosys t(s) | MyIR t(s) | Equiv |
|---|---|---:|---:|---:|---:|---:|---|
| `dec` | pass | 312 | 316 | 1.013 | 3.01 | 3.04 | PASS |
| `int2float` | pass | 443 | 346 | 0.781 | 3.02 | 3.02 | PASS |
| `ctrl` | pass | 306 | 273 | 0.892 | 3.01 | 3.06 | PASS |
| `cavlc` | pass | 1279 | 943 | 0.737 | 3.12 | 3.38 | PASS |
| `priority` | pass | 1907 | 1555 | 0.815 | 3.17 | 6.33 | PASS |
| `router` | pass | 459 | 363 | 0.791 | 3.00 | 3.32 | PASS |
| `i2c` | pass | 2176 | 1898 | 0.872 | 3.20 | 3.75 | PASS |
| `adder` | pass | 2162 | 1655 | 0.765 | 3.17 | 3.73 | PASS |
| `bar` | pass | 5263 | 3408 | 0.648 | 3.64 | 5.27 | PASS |
| `max` | pass | 5383 | 4190 | 0.778 | 3.63 | 6.93 | PASS |

## Summary

- Total benchmarks: **10**
- Passed (pipeline ran ok): **10/10**
- Equiv proven: **10/10** (SAT: 10, structural: 0)
- Optimization (MyIR vs Yosys cells): MyIR smaller on **9**, tie on 0, Yosys smaller on 1
- Geomean cell ratio (MyIR / Yosys, passed only): **0.804**

Notes:
- A ratio < 1.0 means MyIR produces fewer cells than Yosys's `opt -full` after `opt_clean`.
- A ratio > 1.0 means Yosys's native pipeline is currently more aggressive on this benchmark.
- `measure-only` = `--mode measure`: ratio collected, no proof run. `PASS` = full SAT proof (`equiv_simple`). `PASS-struct` = structural proof above `--struct-threshold` cells (identical cell-type histogram + byte-reproducible RTLIL). `timeout`/`timeout-struct` = proof did not finish in time; the ratio is still valid.
