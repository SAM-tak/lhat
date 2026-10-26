# L^ / Luau / Lua VM comparison

CPU: AMD Ryzen 5 2600 Six-Core Processor
OS: Windows-10-10.0.26300-SP0
Compilers: L^: Clang 22.1.3; Luau: Clang 22.1.3; Lua: MSVC 19.51.36260.0
Luau bytecode: O2; Release; PGO/JIT/debugger off.
Host setup: core only; no standard libraries registered; Luau globals sandboxed.
L^ commit: `4bccf17b4164f0448c2ecac080030ad5977ae8ca`; local changes: True.
Luau commit: `32c28089305f488f901aaf1dbf3fd1cb65fa3d11`.
Official Lua: 5.5.1 (pinned release, source SHA-256 in results.json).
Comparison mode: mixed compilers (selected build per VM); this is separate from the uniform-compiler baseline.

Times are microseconds per workload round. Brackets show the range of process medians, not a confidence interval.
Ratios = L^ time / the named VM time; below 1 means L^ is faster. Each round includes the setup specified in README.md.

| Case | L^ µs [min–max] | Luau µs [min–max] | Lua µs [min–max] | L^ / Luau | L^ / Lua |
| --- | ---: | ---: | ---: | ---: | ---: |
| arithmetic | 97.66 [96.32–100.89] | 79.88 [79.64–80.70] | 75.31 [74.83–77.44] | 1.223 | 1.297 |
| calls | 487.70 [485.39–496.05] | 235.69 [234.00–236.50] | 260.96 [258.60–274.98] | 2.069 | 1.869 |
| array | 282.94 [279.47–305.59] | 64.68 [63.79–70.10] | 86.03 [84.65–87.62] | 4.374 | 3.289 |
| fields | 1388.43 [1365.64–1398.53] | 154.13 [153.11–190.31] | 342.50 [338.99–352.10] | 9.008 | 4.054 |
| allocation | 491.51 [482.30–531.73] | 69.25 [69.01–71.93] | 264.54 [259.03–294.95] | 7.097 | 1.858 |
| entities | 3474.53 [3428.27–3560.01] | 300.95 [299.02–346.39] | 655.52 [641.27–671.01] | 11.545 | 5.300 |

Cold runs use a fresh process, one workload round, zero warmups. OS file caches are not flushed.
Main → first result excludes OS process creation; process wall time includes creation, JSON output and teardown.

| Case | Main → result L^ / Luau / Lua ms | Process wall L^ / Luau / Lua ms | Peak process L^ / Luau / Lua MiB |
| --- | ---: | ---: | ---: |
| arithmetic | 0.58 / 0.52 / 0.33 | 17.42 / 18.70 / 17.06 | 4.92 / 5.29 / 4.37 |
| calls | 1.01 / 0.71 / 0.52 | 17.77 / 18.73 / 17.31 | 4.94 / 5.31 / 4.37 |
| array | 0.81 / 0.55 / 0.37 | 17.64 / 18.41 / 17.52 | 4.94 / 5.32 / 4.37 |
| fields | 2.23 / 0.68 / 0.64 | 25.44 / 25.96 / 21.07 | 4.95 / 5.30 / 4.37 |
| allocation | 1.28 / 0.67 / 0.60 | 22.04 / 22.45 / 21.15 | 5.05 / 5.35 / 4.39 |
| entities | 4.15 / 0.82 / 0.91 | 21.53 / 18.89 / 17.51 | 5.08 / 5.29 / 4.40 |

Peak process memory includes compiler and VM setup; this is not VM heap allocation or a steady-state memory measurement.
L^ check_load_ns includes source loading, parsing and strict checking. Luau/Lua compile_ns includes parsing and bytecode compilation, with no type analysis.
These preparation timings are retained in results.json and are not presented as equivalent frontend work.
