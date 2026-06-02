# EPFL Benchmark Comparison Report

| Benchmark | Status | Yosys cells | MyIR cells | Ratio | Yosys t(s) | MyIR t(s) | Equiv |
|---|---|---:|---:|---:|---:|---:|---|
| `dec` | pass | 312 | 312 | 1.000 | 5.83 | 4.08 | PASS |
| `int2float` | pass | 443 | 443 | 1.000 | 6.46 | 4.25 | PASS |
| `priority` | pass | 1907 | 1907 | 1.000 | 16.49 | 8.44 | PASS |
| `adder` | fail-equiv | 2162 | 2162 | 1.000 | 17.97 | 9.00 | FAIL |

## Summary

- Total benchmarks: **4**
- Passed (full pipeline): **3/4**
- Formal equiv PASS:      **3/4**
- Geomean cell ratio (MyIR / Yosys, passed only): **1.000**

Notes:
- A ratio < 1.0 means MyIR produces fewer cells than Yosys's `opt -full` after `opt_clean`.
- A ratio > 1.0 means Yosys's native pipeline is currently more aggressive on this benchmark.
