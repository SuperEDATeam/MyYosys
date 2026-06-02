# EPFL Benchmark Comparison

End-to-end harness comparing **MyIR's optimization pipeline** against
**Yosys's native `opt -full`** on the
[EPFL combinational benchmark suite](https://github.com/lsils/benchmarks),
with formal equivalence verification.

This is the primary "is the AST route any good" evidence stream for
the project; the MyIR path goes through
`myir_ast_import -> myir_opts -fixpoint -> myir_export`.

## Layout

```
tests/epfl/
  README.md            (this file)
  benchmarks.txt       which benchmarks to run, with tiers
  templates/
    yosys_baseline.ys.tmpl   gold pipeline (Yosys native opt -full)
    myir_pipeline.ys.tmpl    gate pipeline (MyIR full optimization stack)
    equiv.ys.tmpl            equiv_make + equiv_induct check
  run_compare.py       driver script
  results/             generated output (CSV / Markdown / per-bench logs)
  lsils_benchmarks/    (optional) git submodule of the EPFL suite
```

## Prerequisites

1. **Python 3.9+** (uses `dataclasses`, `typing` from stdlib only).
2. **Built MyVSYosys.exe** (Release recommended; Debug works but is much
   slower).  The script searches the typical VS output paths:
   ```
   x64\Release\MyVSYosys.exe
   x64\Debug\MyVSYosys.exe
   Release\MyVSYosys.exe
   Debug\MyVSYosys.exe
   MyVSYosys\x64\Release\MyVSYosys.exe
   MyVSYosys\x64\Debug\MyVSYosys.exe
   ```
   You can also point at an explicit binary with `--yosys` or
   the environment variable `YOSYS_BIN`.
3. **EPFL benchmarks** checked out somewhere.

## Getting the EPFL benchmarks

Option A -- git submodule (recommended for reproducibility):

```powershell
git submodule add https://github.com/lsils/benchmarks tests/epfl/lsils_benchmarks
git submodule update --init
```

Option B -- local clone, anywhere on disk:

```powershell
git clone https://github.com/lsils/benchmarks D:\benchmarks-epfl
# then pass --epfl-root to run_compare.py
```

## Quick start

After building MyVSYosys.exe in Release, from the repo root:

```powershell
python tests/epfl/run_compare.py --filter "dec|int2float|adder|priority" -j 4
```

Outputs are written under `tests/epfl/results/`:

- `epfl_results.csv` -- machine-readable per-benchmark table
- `epfl_results.md`  -- human-readable Markdown report with summary
- `tests/epfl/results/<bench_name>/` -- per-benchmark scratch dir
  containing `yosys.log`, `myir.log`, `equiv.log` and the generated
  `.ys` scripts.  Useful for post-mortem when a bench fails.

## Command-line reference

```
python tests/epfl/run_compare.py [options]

  --yosys PATH          path to yosys / MyVSYosys executable
                        (default: auto-detect or $YOSYS_BIN)
  --epfl-root PATH      EPFL benchmark root dir
                        (default: tests/epfl/lsils_benchmarks)
  --bench-list FILE     benchmark list file
                        (default: tests/epfl/benchmarks.txt)
  --filter REGEX        run only benches matching this regex
  --include-mtm         enable the very large MtM benchmarks
                        (skipped by default to avoid OOM)
  --no-equiv            skip the formal equivalence step
                        (faster, stat-only comparison)
  --with-reliable       reserved -- run myir_reliable as a pre-check
  --timeout SECS        per-yosys-invocation timeout (default 600)
  -j N                  run N benchmarks in parallel (default 1)
  --keep-intermediate   keep .v / .rtlil intermediates per bench
                        (default: delete after equiv passes)
  --out-dir DIR         where to write results (default: results/)
```

## Reading the report

Every benchmark line has:

| Column      | Meaning |
|-------------|---------|
| status      | `pass` / `fail-baseline` / `fail-myir` / `fail-equiv` / `crash` |
| yosys cells | cell count after Yosys `opt -full` + `clean` |
| myir cells  | cell count after MyIR pipeline + `opt_clean` |
| ratio       | `myir_cells / yosys_cells` (lower = MyIR wins) |
| equiv       | `PASS` / `FAIL` / `skip`  -- result of equiv_make/induct |
| t(s)        | wall-clock seconds for each phase |

The summary at the bottom prints geomean of the ratio over passing
benchmarks; that's the headline number to quote in the report.

## Windows / PowerShell notes

- All paths in templates use **forward slashes** to avoid Yosys
  interpreting backslashes as escape characters in command arguments.
  The Python driver normalizes input paths automatically.
- Long benchmark paths can exceed the default 260-char limit on some
  Windows configurations.  Either enable long path support in
  Windows or keep the repo on a short root like `D:\MyVSYosys-pub`.
- If the yosys binary is wrapped (e.g. `MyVSYosys.exe`), make sure
  it is the **same build** that contains all `myir_*` passes;
  otherwise `myir_ast_import` will fail with "command not found".

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `--epfl-root '...' does not exist` | submodule not initialized | `git submodule update --init` or pass `--epfl-root` to a clone |
| `Could not locate yosys/MyVSYosys` | release not built | build Release in VS or pass `--yosys` |
| `myir_pipeline` fails with `myir_ast_import is not a known command` | wrong binary picked up (system Yosys, not your build) | set `YOSYS_BIN` to your MyVSYosys.exe |
| `equiv` fails with `equiv_status -assert` exit 1 | MyIR really did produce non-equivalent RTLIL | inspect `results/<bench>/equiv.log`; rerun MyIR side with `myir_export -strict` to surface dropped expressions |
| All benches timeout in parallel mode | each yosys invocation pegs a CPU; `-j N` only helps when N <= cores; large benches still need a lot of single-thread time | drop `-j`, raise `--timeout` |

## Adding more benchmarks

Edit [benchmarks.txt](benchmarks.txt).  Lines beginning with `!mtm`
require `--include-mtm`; everything else is run by default.  Use
`--filter` to subset interactively without editing the file.
