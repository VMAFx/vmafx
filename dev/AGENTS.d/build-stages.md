---
paths:
  - dev/Containerfile
  - scripts/ci/tests/test_dev_image_build_stages.py
invariant: libvmaf, FFmpeg build trees only in libvmaf-compile, ffmpeg-compile; image stages COPY their DESTDIR staging roots.
---
<!-- markdownlint-disable MD013 -->
# Build stages keep build trees out of image layers

- Chain: `libvmaf-deps` (sources, ONNX Runtime, nv-codec-headers) ->
  `libvmaf-compile` (meson setup + ninja, `DESTDIR=/stage/libvmaf`
  install) -> `codec-deps` (`COPY --from=libvmaf-compile /stage/libvmaf/ /`,
  backend probe, encoder libraries) -> `ffmpeg-compile` (patched FFmpeg,
  `make install DESTDIR=/stage/ffmpeg`) -> `libvmaf-build`
  (`COPY --from=ffmpeg-compile`, encoder probe). `go-build`, `dev-mcp`
  derive from `libvmaf-build`; CI gate builds `libvmaf-build` (ADR-0819).
- Build tree removed in later `RUN` stays in build layer (ADR-0790
  pattern: Meson tree 8.25 GB, FFmpeg tree 1.1 GB in every image). Never
  build and delete across layers in image stage; build in `*-compile`
  stage, copy staging root.
- Nothing after install reads `core/build` or `/build/ffmpeg`; `/build/vmaf`
  sources stay (editable installs, entrypoint, healthcheck, models,
  testdata).
- `meson setup core/build core` line stays verbatim
  (`test_dev_container_reference_build_flags.py`).
- `test_dev_image_build_stages.py` walks FROM chain of `dev-mcp`,
  `libvmaf-build`; refuses compile step there.
