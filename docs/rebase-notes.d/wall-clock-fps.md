## Wall-clock fps in report output (2026-10-26)

`fix/wall-clock-fps` (ADR-3377). Adds monotonic wall-clock FPS (`wall_fps`)
beside the process CPU time `fps` in XML and JSON report outputs.

- `core/src/feature/feature_collector.h`: adds `struct { struct timespec begin, end; } wall_timer;`
  to `VmafFeatureCollector`.
- `core/src/feature/feature_collector.cpp`: sets `wall_timer.begin` and
  `wall_timer.end` using `std::chrono::steady_clock` at the same points as
  `timer.begin` and `timer.end` in `vmaf_feature_collector_append()`.
- `core/src/libvmaf.c`: adds `output_wall_fps()` beside `output_fps()` with
  the same ADR-0606 defensive guards (`pic_cnt == 0 || elapsed_sec <= 0.0 -> 0.0`).
  Routes `wall_fps` to `output_write()` and the XML/JSON writers. Exposes
  `vmaf_output_fps_for_test()`, `vmaf_output_wall_fps_for_test()`, and
  `vmaf_context_set_pic_cnt_for_test()` in `core/src/libvmaf_priv.h`.
- `core/src/output.h` & `core/src/output.cpp`: `vmaf_write_output_xml()` and
  `vmaf_write_output_json()` take `double wall_fps` alongside `double fps`.
  XML emits `<fyi fps="%.2f" wall_fps="%.2f" />`; JSON emits `"wall_fps": %.2f`
  directly after `"fps"` (serialised as `null` on NaN/Inf).
- `core/src/vmafx/report_verify.c`: adds `"wall_fps"` to `environment()` so
  that report verification treats wall-clock throughput as run-varying metadata.
