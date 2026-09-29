- **SYCL: the native Windows build runs its kernels.** A Windows MSVC build
  linked `vmaf.exe` and the tests with `link.exe`, which ignored `-fsycl` and
  never registered the SYCL device images, so every SYCL kernel submit failed
  with `No kernel named ... was found` and 47 of the 50 SYCL tests failed on an
  Arc B580. The build now runs one `icpx -fsycl -fsycl-link` step over the SYCL
  objects and links its registration object into every program that uses the
  SYCL backend, including static consumers of `vmaf.lib`. On an Arc B580 and a
  UHD 770 all SYCL tests pass and all 19 SYCL extractors agree with the CPU
  within the parity gate. The `Windows MSVC+SYCL` CI leg now checks that the
  kernels are registered, and [SYCL on Windows](docs/backends/sycl/windows.md)
  documents the native build (ADR-1364, `T-SYCL-WINDOWS-MSVC-KERNELS-UNREGISTERED-2026-09-29`).
- **CI: Windows test lanes gate their tests again.** `scripts/ci/run_meson_test.py`
  replaced itself with `meson test` through `os.execvp`, which on Windows starts
  Meson and ends the runner with status 0 at once. The Windows MinGW64 and ARM64
  MSVC lanes reported success after 17 to 19 tests, over failing ones. The runner
  now waits for Meson on Windows and returns its status; four Windows test-harness
  failures it had hidden are fixed (`T-CI-WINDOWS-MESON-TEST-RUNNER-EXIT-0-2026-09-29`,
  `T-TEST-WINDOWS-HARNESS-MASKED-FAILURES-2026-09-29`).
- **Parity gate: the `cambi` cell compares scores.** `cross_backend_parity_gate.py`
  and `cross_backend_vif_diff.py` looked the score up as `Cambi_feature_cambi_score`,
  but `vmaf --json` writes it as `cambi`, so the cell stopped with `KeyError`
  (`T-CI-PARITY-GATE-CAMBI-KEY-2026-09-29`).
