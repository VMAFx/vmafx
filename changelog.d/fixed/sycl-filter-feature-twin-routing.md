- **FFmpeg's `libvmaf_sycl` runs `feature=` names on their SYCL twins.** The
  filter registered each name as written, so `feature=name=psnr` ran the CPU
  extractor on software input and was refused on QSV zero-copy input even
  where its twin runs there. It now resolves names through
  `vmaf_feature_backend_twin()`, as the `vmaf` CLI does: on zero-copy input a
  feature without a usable twin fails at configuration and names the reason,
  on software input the filter warns and computes it on the CPU. QSV zero-copy
  accepts NV12 and P010 surfaces only and names any other format
  ([ADR-3360](docs/adr/3360-sycl-filter-twin-routing.md);
  [Using VMAF with FFmpeg](docs/usage/ffmpeg.md#how-feature-names-are-resolved-in-libvmaf_sycl)).
  `scripts/test/zerocopy-e2e.sh` checks the zero-copy path against host upload
  and the CPU on an Intel GPU
  ([SYCL zero-copy testing](docs/development/sycl-zerocopy-testing.md)).
