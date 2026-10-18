<!-- markdownlint-disable MD013 MD060 -->
# Research-1442: What changes when float ADM divides instead of using the processor's reciprocal estimate

- **Status**: Active
- **Workstream**: [ADR-1442](../adr/1442-float-adm-reference-divides.md), [ADR-1420](../adr/1420-cuda-float-adm-cpu-arithmetic.md), [ADR-1317](../adr/1317-golden-gate-build-isolation.md)
- **Last updated**: 2026-10-02

## Question

The maintainer's condition for making the IEEE quotient the float ADM
reference was that the Netflix golden gate holds. Does it? What does the
division cost on the CPU, how far do the scores move, and does the CUDA twin
still return the CPU's bits once its probed estimate is replaced by a
division?

## Sources

- `core/src/feature/adm_options.h` (`ADM_OPT_RECIP_DIVISION`),
  `core/src/feature/adm_tools.c` (`DIVS()`, `rcp_s()`,
  `adm_decouple_band_s()`), at master `953cf6ea6` (before) and on
  `fix/float-adm-reference-divides` (after).
- Reference workstation: Ryzen 9 9950X3D, RTX 4090, gcc 16.2.1, glibc 2.44, CUDA 13.4.
  CPU builds: `meson setup <dir> core -Denable_cuda=false -Denable_sycl=false
  -Db_lto=false --buildtype release`, one from each tree. Golden build:
  `scripts/ci/setup-golden-build.sh core/build-golden`.
- Fixtures: Netflix 576x324 at 8 bits (48 frames) and 10, 12, 16 bits (3
  each), both 1080p checkerboard pairs (3 each), BBB 3840x2160 (50 frames).
  For timing at 1920x1080, a 60-frame centre crop of the BBB pair.

## Findings

1. **Where the division is.** The only user of `DIVS()` in a default build is
   `adm_decouple_band_s()`: `k = DIVS(t, o + eps)`, once per band sample
   (`adm_angle_flag_s()` uses it only without `ADM_OPT_AVOID_ATAN`, which is
   defined). The AVX2, AVX-512 and NEON float ADM files implement the DWT,
   the CSF and two reductions, not the decouple; none contains a reciprocal
   intrinsic. `libvmaf.so` of the before build has 7 `rcpss` instructions, of
   the after build none, and `adm_decouple_s` then has 7 `divss`. MSVC (no
   `__SSE2__` macro) and ARM builds took the dividing branch already.

2. **Golden gate.** The test selection of `make test-netflix-golden`
   (`quality_runner_test.py`, `feature_extractor_test.py`, `vmafexec_test.py`,
   `vmafexec_feature_extractor_test.py`, `result_test.py`, `-m "not slow"`)
   against `core/build-golden`, built by `setup-golden-build.sh` from each
   tree:

   | Golden build | Passed | Skipped | Failed |
   |---|---:|---:|---:|
   | master `953cf6ea6` (estimate) | 271 | 12 | 0 |
   | this branch (division) | 271 | 12 | 0 |

   The `make` target itself stops in a fresh worktree before it runs a test:
   it creates a `.venv` with the build tools only and then calls
   `python3 -m pytest` from it. The same pytest command was run with the main
   checkout's interpreter.

3. **CPU cost.** `--backend cpu --threads 1 --feature float_adm`,
   `(t(N) - t(2)) / (N - 2)`, alternating before and after, host load
   average 25 to 50 (other jobs on the machine). Process CPU time, median of
   five runs:

   | Size | Path | Before (ms/frame) | After (ms/frame) | Change |
   |---|---|---:|---:|---:|
   | 576x324 | scalar | 1.50 | 1.32 | -12 % |
   | 576x324 | AVX2 | 1.49 | 1.35 | -9 % |
   | 576x324 | AVX-512 | 1.49 | 1.33 | -11 % |
   | 1920x1080 | scalar | 21.3 | 18.2 | -15 % |
   | 1920x1080 | AVX2 | 23.1 | 20.7 | -10 % |
   | 1920x1080 | AVX-512 | 18.8 | 18.6 | -1 % |
   | 3840x2160 | scalar | 84.9 | 90.4 | +7 % |
   | 3840x2160 | AVX2 | 83.6 | 76.8 | -8 % |
   | 3840x2160 | AVX-512 | 78.1 | 72.4 | -7 % |

   Wall time, median of three, gave -16 % to +1 % with the same scatter. The
   decouple is the same scalar code under every `--cpumask`, so the spread
   between rows is the host's load, not the instruction set; the one positive
   row has single runs from 66 to 98 ms. Nothing here is above the 10 % that
   would have stopped the change, and the direction is the other way.

4. **Score movement**, before against after, `--precision max`, 16 threads:

   | Outputs | Changed | Largest change |
   |---|---:|---:|
   | `float_adm` scores (7 per frame) | 147 of 791 | 1.3e-7 (`adm_scale2`) |
   | `float_adm` with `debug=true` (18 per frame) | 251 of 2034 | 6.1e-5 (`adm_num_scale1`, an fp32 sum near 294: two float steps) |
   | `float_adm`, `adm_enhn_gain_limit=1.2` | 160 of 791 | 1.6e-7 |
   | `vmaf_float_v0.6.1` per frame | 33 of 113 | 1.1e-5 |
   | `vmaf_float_v0.6.1neg` per frame | 35 of 113 | 1.2e-5 |
   | `vmaf_float_4k_v0.6.1` per frame | 33 of 113 | 9.2e-6 |

   Per fixture, `float_adm` scores: Netflix 8-bit 79 of 336 (1.3e-7); 10, 12
   and 16 bits 5 of 21 each (1.0e-7); checkerboard 1 px 4 of 21 (1.2e-7);
   checkerboard 10 px 0 of 21; BBB 3840x2160 49 of 350 (1.0e-7). The clip
   mean of the three models moves by at most 2.7e-6
   (`vmaf_float_v0.6.1neg`, Netflix 10-bit). The fixed-point `adm` and the
   default models are untouched: `vmaf_v0.6.1` output is identical apart
   from the timing line.

5. **Instruction sets agree.** `float_adm` with `debug=true` under
   `--cpumask` 0 (AVX-512), 48 (AVX2) and 4294967295 (scalar) is identical on
   every output of the Netflix pair at 8, 10 and 16 bits, both checkerboard
   pairs and 10 BBB frames, in the before build and in the after build.

6. **Fork snapshots.** `testdata/scores_cpu_*.json` and
   `testdata/netflix_benchmark_results.json` are produced with
   `vmaf_v0.6.1`, which uses the fixed-point `adm`. No file under
   `testdata/` holds a float ADM value, so none was regenerated.

7. **The CUDA twin.** `fadm_divs()` is `__fdiv_rn(n, d)`; the kernel fatbins
   take no `--use_fast_math` and nvcc defaults to `-prec-div=true`, and the
   intrinsic does not depend on either. RTX 4090, `--precision max`, against
   `--backend cpu` of the same binary: 791 of 791 scores and 2034 of 2034
   outputs with `debug=true` identical; `adm_enhn_gain_limit=1.2` 378 of 378
   on the Netflix pair, a checkerboard pair and the 10-bit pair; the gate
   reports 0 on 200 BBB frames and on the Netflix pair at tolerance 0;
   `test_cuda_float_adm_parity` 17 of 17.

   Timing against master's twin, host load average 9 to 12: a run of the twin
   alone, `(t(52) - t(2)) / 50` on BBB 3840x2160, seven alternating pairs,
   1.83 and 1.96 ms per frame, paired +0.09 (+0.03 to +0.18); 576x324 0.14
   and 0.05 ms (inside the noise). One more instance of the twin in the same
   process, nine against one over 60 frames: 0.98 ms per frame before, 0.83
   after. The table lookup and the Newton step cost more than the division.
   The 10 ms probe at start and 16 KB of device memory are gone.

8. **The value test has teeth on this host.** Of two million random decouple
   inputs (`o` in 0.5 to 200, `k` in 0 to 1), 30 % give another restored
   value with the estimate than with the quotient here.
   `test_float_adm_device_math` holds four of them; with `rcp_s()` put back
   into `adm_tools.c` it fails on the first
   (`quotient=c19e4395 cpu=c19e4396`), and
   `test_float_adm_divides_contract.py` fails as well.

## Open questions

- A second x86 vendor was never measured with the estimate, so the size of
  the cross-host difference this removes is known only by its mechanism and
  by the 1.3e-7 between this host's estimate and the quotient.
- The cpu-lane clang-tidy ratchet, run in full on this tree, reports 13
  findings above its baseline in `core/tools/vmaf.cpp`,
  `core/src/picture_pool.cpp`, `core/src/read_json_model.cpp`,
  `core/test/test_psnr_hvs_score.c` and
  `core/test/test_read_pictures_failure_ownership.c`, none touched here.
