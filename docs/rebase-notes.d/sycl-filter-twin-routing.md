## SYCL zero-copy hardening: twin routing, timing log, e2e harness (2026-10-05)

`pr/sycl-zerocopy-hardening`, [ADR-3360](adr/3360-sycl-filter-twin-routing.md)
(and ADR-3359 above); ported from the Tualua fork's zero-copy work without its
chroma import, which is post-1.0 ([ADR-1685](adr/1685-post-1-0-embedding-zero-copy-milestone.md)).

- `ffmpeg-patches/0005-libvmaf-add-libvmaf-sycl-filter.patch`:
  `use_feature_sycl()` / `use_feature()` and `route_twins` route `feature=`
  names through `vmaf_feature_backend_twin()`; `config_props_sycl()` refuses
  QSV `sw_format`s other than NV12 / P010. A refresh of the series keeps both;
  `ffmpeg-patches/test/check-sycl-feature-routing.sh` fails otherwise. The
  import-failure branch keeps master's retry-then-fail form
  ([ADR-1761](adr/1761-sycl-filter-import-retry-then-fail.md)).
- `core/src/sycl/common.cpp`: `vmaf_sycl_print_timing()` uses `vmaf_log` at
  INFO (`core/src/sycl/AGENTS.md`).
- `scripts/test/zerocopy-e2e.sh`, `zerocopy_e2e_compare.py`: stage 1 is the set
  ADR-1688 admits on the luma-only path. A change to
  `reads_shared_luma_only()` answers moves cases between stages in
  `PARITY_STAGE`; the e2e legs never use `-frames:v` / `-t`.
- No Netflix golden-data or public C API impact.
