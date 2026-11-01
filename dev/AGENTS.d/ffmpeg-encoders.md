---
paths:
  - dev/Containerfile
  - ffmpeg-patches/series.txt
invariant: Source-built SVT-AV1 and libvvenc; vendored AMF; QSV dual runtime; hardware encoder flags in FFmpeg configure.
---
<!-- markdownlint-disable MD013 -->
# FFmpeg encoder exposure invariants (ADR-0540)

These constraints must survive every rebase. Each corresponds to
real encoder or FFmpeg integration path that `vmaf-tune compare`
predicate would silently skip, or that dev-container FFmpeg build
would fail to compile:

1. **SVT-AV1 must be built from source; apt `libsvtav1-dev` package
   NOT sufficient.** Ubuntu's `libsvtav1-dev` (1.7.0+dfsg-2build1)
   omits `SvtAv1Enc.pc` (verified 2026-05-18 against `ubuntu:24.04`).
   FFmpeg's `require_pkg_config libsvtav1
   SvtAv1Enc ...` probe therefore fails. SVT-AV1 cloned from
   `https://gitlab.com/AOMediaCodec/SVT-AV1.git` at pinned tag
   (`v2.1.0` at time of writing), built with cmake under
   `/usr/local/`. `cmake --install` writes `SvtAv1Enc.pc` to
   `/usr/local/lib/pkgconfig/` as side effect. Do NOT replace with
   distro package.

   **libaom intentionally NOT enabled.** Fork's
   `ffmpeg-patches/0001` references libaom `aom_roi_map_t` fields
   that don't exist in any released libaom version. SVT-AV1 covers
   production AV1 lane. Re-enabling libaom requires first fixing
   patch 0001's ROI helper to either target real libaom version or
   gate ROI bridge behind version probe.
2. **`libvvenc` (Fraunhofer VVC reference) must be built from
   source, installed under `/usr/local`.** Package not in Ubuntu
   apt. Pin to release tag (`v1.14.0` as of ADR-0568; bumped from
   `v1.12.0` 2026-05-18) so future rebases get deterministic build.
   Configure-time check = `check_pkg_config(libvvenc, ...)`, needs
   `.pc` file `VVENC_ENABLE_INSTALL=ON` ships.
3. **AMF headers vendored from upstream `GPUOpen-Libraries-
   AndSDKs/AMF` repo (header-only).** FFmpeg's `--enable-amf` needs
   only headers at compile time;
   `libamfrt64.so` runtime resolution = host-side. Do NOT try
   installing `libamfrt64.so` from apt — lives in proprietary
   `amdgpu-pro` userspace, not packaged.
4. **QSV needs both oneVPL dispatcher and GPU runtime.**
   `libvpl-dev` provides `libvpl.so.2`, lets FFmpeg compile
   `--enable-libvpl`, but doesn't provide Gen implementation
   (`libmfx-gen.so`) that creates Arc/iGPU MFX session at runtime.
   `dev/Containerfile` builds `intel/vpl-gpu-rt` at pinned
   `VPL_GPU_RT_TAG`, installs it into `/usr/lib/x86_64-linux-gnu/`,
   path searched by Ubuntu's `libvpl.so.2` dispatcher. Do NOT move
   it back to `/usr/local/lib` without also preserving dispatcher
   discovery, or QSV will regress to
   `Error creating a MFX session: -9` while still appearing in
   `ffmpeg -encoders`.
5. **FFmpeg configure line carries all of `--enable-nvenc
   --enable-cuda-nvcc --enable-libvpl --enable-amf`, in addition to
   software codec flags.** Dropping any one silently disappears
   hardware-encoder family from `ffmpeg -encoders` listing, breaks
   `vmaf-tune compare` sweep. build-time encoder probe at end
   of stage 3.5 fails if any promised encoder is missing; listing    compiled hardware encoder does not require device. Do NOT add
   `--enable-libnpp`. FFmpeg n9.0.2 has
   removed libnpp support; option is compatibility no-op that
   emits `libnpp has been removed and enabling it does nothing`.
   Keeping it absent preserves warning-clean configure output.
   `scale_cuda` (built via `--enable-cuda-nvcc`) covers GPU-scale
   pipeline. Reconsider only if future FFmpeg release restores    real libnpp probe and matching CUDA contract is validated.
   AMF and FFmpeg release checkouts use
   `scripts/ci/checkout-annotated-tag.sh`. Direct `git clone --depth=1
   --branch <tag>` emits a warning for both annotated tags in the image's Git
   version and violates zero-diagnostic build contract.
6. **FFmpeg vmafx filter cleanup destroys context and releases device.**
   `ffmpeg-patches/src/vf_vmafx.c` (patch `0004`) cleans up in `release_all()`:
   it destroys the VMAFx context via `vmafx_context_destroy(s->context, NULL)`
   and releases `s->vdev` via `vmafx_device_unref(s->vdev)`.
