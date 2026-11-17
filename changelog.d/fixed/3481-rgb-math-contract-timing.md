- **Cache compiler probes, log phases and warm up compilers for
  `test_vmafx_rgb_math_contract`.**
  `compile_frame_import` re-ran `is_gnu_gcc` and `probe_c_std` on every call,
  and a timed-out run reported neither which compiler step stalled nor the
  Meson test log on the Windows ARM64 MSVC job. `is_gnu_gcc` and `probe_c_std`
  now cache per resolved compiler executable, and every compiler subprocess
  logs `start` and `done` phase lines to `stderr` with flushing. The
  `windows-arm64` job warms the GNU compilers the test resolves (`--warm-up`)
  before the fast suite and uploads `core\build\meson-logs\testlog.txt` on
  every non-cancelled run.
