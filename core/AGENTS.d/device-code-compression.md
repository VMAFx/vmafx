---
paths:
  - core/src/meson.build
  - core/meson_options.txt
  - core/src/check_device_compression.py
  - core/test/test_device_code_compression.py
invariant: Every device compile takes its backend's compression list; the build refuses raw device code.
---
<!-- markdownlint-disable MD013 MD060 -->
# Device code compression policy (ADR-1590)

## Rebase-sensitive invariants

- **One compression list per backend (ADR-1590)**: `core/src/meson.build`
  holds `BEGIN/END VMAF {CUDA,HIP,SYCL} device code compression policy`
  blocks, each defining one list: `cuda_compress_args`
  (`-Xfatbin=-compress-all --compress-mode=size`, `--no-compress` when off),
  `hip_compress_args` / `sycl_compress_args` (`--offload-compress
  --offload-compression-level=22`). Option `compress_device_code` (default
  `true`) in `core/meson_options.txt`; on with compiler lacking compression
  (clang CUDA via `enable_nvcc=false`, AdaptiveCpp, compiler without flag) =
  configure `error()`, never silent skip.
- **Use sites**: nvcc fatbin `custom_target` (`cu_ptx_target_*`); every
  `[hipcc_exe, '--genco']` command in `core/src/meson.build` and
  `core/test/meson.build` (test probes too); SYCL AOT compile
  (`sycl_compress_args + sycl_icpx_aot_base_args`, toolchain and per-TU skip
  path); `sycl_link_args` (final link generates SPIR-V image, AOT builds
  too); MSVC device link (`sycl_device_link_args`). New device compile site =
  add list. Compression flags spelled only inside blocks.
- **Build-time check**: `cuda_device_compression_check`,
  `hip_device_compression_check`, `sycl_device_compression_check` run
  `core/src/check_device_compression.py` (reuses ELF / zstd readers of
  `core/src/sycl/check_aot_image.py`); raw cubin / PTX, plain offload bundle,
  or raw SYCL image section = build failure.
- **Not adopted**: nvcc `--concat` (NVIDIA documents no driver floor; R580
  promise of CUDA 13 builds). Turn on only with documented or measured floor
  plus new ADR.
- Guard: `core/test/test_device_code_compression.py` (policy contract +
  checker fixtures, suite `fast`). Device-code identity proof in ADR-1590:
  decompressed cubins / PTX byte-identical, HIP code objects identical
  except `__hip_cuid_*` name.
