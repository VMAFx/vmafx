<!-- markdownlint-disable MD013 MD060 -->
# ADR-0540: dev-MCP container FFmpeg ships AV1 (SVT/aom) + VVenC + hardware encoders (NVENC, oneVPL/QSV, AMF)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: container, dev-experience, ffmpeg, codecs, av1, vvc, nvenc, qsv, amf, fork-local

## Context

The `vmaf-dev-mcp` container (`dev/Containerfile`) is the project-mandated
default execution environment for vmaf / vmaf-tune / ai / MCP work
(CLAUDE.md §12 rule 15, ADR-0496). Despite ADR-0514 making every libvmaf
GPU backend runnable inside the container, the in-image FFmpeg was
configured with only four video encoders:

```text
--enable-libx264 --enable-libx265 --enable-libvpx --enable-libdav1d
```

`libdav1d` is an AV1 *decoder*, not an encoder. The effective encoder set
was `libx264`, `libx265`, `libvpx-vp9` — three CPU encoders, no AV1
encoders, no VVC encoder, and zero hardware encoders, on a host with
three GPUs (NVIDIA RTX 4090 + Intel Arc A380 + AMD gfx1036) that the
ADR-0514 work specifically unlocked for kernel dispatch.

The user-visible failure was the `vmaf-tune compare` BBB e2e v10 sweep:
when the operator asked for `--encoders
libx264,libx265,libsvtav1,libvpx-vp9,h264_nvenc,hevc_nvenc,...`, the
compare predicate dropped every encoder past the third with the
stable error string:

```text
hardware encoder not available: libsvtav1 not compiled into ffmpeg
hardware encoder not available: h264_nvenc not compiled into ffmpeg
...
```

The probe is correct: `tools/vmaf-tune/src/vmaftune/compare.py::
probe_encoder_available` runs `ffmpeg -hide_banner -encoders` and scans
for each requested encoder token (`_encoder_listed`). When the encoder
is absent from the FFmpeg build, it returns an `ok=False` row with the
"not compiled into ffmpeg" diagnostic rather than crashing the whole
sweep. The fix therefore is on the container side: ship an FFmpeg that
the probe actually finds the encoders in.

Three codec families are missing:

1. **Software AV1.** Two industry-standard codepaths exist —
   `libsvtav1` (Intel/Netflix Scalable Video Tech, the fast lane) and
   `libaom-av1` (alliance reference, the slow but widely-tooled lane).
   Both are required because vmaf-tune compare sweeps include them as
   first-class members of the codec adapter registry
   (`tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py`); the
   `corpus.py` HDR pipeline branches on encoder identity to pick CRF /
   preset translations (`hdr.py::_hdr_args_svtav1`).
2. **Software VVC (H.266).** The Fraunhofer reference encoder
   `libvvenc` is referenced throughout the vmaf-tune CLI surface
   (`cli.py:203`, `cli.py:549`, `compare.py` examples). It is not in
   Ubuntu's apt repo and must be built from source.
3. **Hardware encoders (NVENC, oneVPL/QSV, AMF).** The host has all
   three GPU vendors. NVENC unlocks `h264_nvenc` / `hevc_nvenc` /
   `av1_nvenc` (AV1 added in Ada Lovelace, the RTX 4090 supports it);
   Intel oneVPL (the modern replacement for sunsetted libmfx) unlocks
   `h264_qsv` / `hevc_qsv` / `av1_qsv` on the Arc A380; AMD AMF
   unlocks `h264_amf` / `hevc_amf` / `av1_amf` on gfx1036 (subject to
   the amdgpu-pro userspace constraint discussed below).

## Decision

Patch `dev/Containerfile` stage 3.5 to install the missing codec
dependencies and pass the matching `--enable-*` flags to FFmpeg's
configure. Concretely:

1. **Distro packages added to stage 3.5 apt step**: `libvpl-dev`
   (already in stage 1 but re-listed at the FFmpeg-configure layer
   for documentation). `libvpl-dev` ships `vpl.pc` correctly.
2. **SVT-AV1 built from source** (NOT from Ubuntu's
   `libsvtav1-dev` apt package). `libsvtav1-dev` omits
   `SvtAv1Enc.pc` — a Debian packaging quirk verified 2026-05-18
   against a clean `ubuntu:24.04` install. FFmpeg's
   `require_pkg_config libsvtav1 SvtAv1Enc ...` configure probe
   therefore fails even with the library physically present.
   SVT-AV1 is cloned from
   `https://gitlab.com/AOMediaCodec/SVT-AV1.git` at tag `v2.1.0`,
   built with CMake (`-DBUILD_SHARED_LIBS=ON -DBUILD_APPS=OFF
   -DBUILD_TESTING=OFF`), installed under `/usr/local`, source
   tree deleted, `ldconfig` refresh. Upstream `cmake --install`
   writes `SvtAv1Enc.pc` to `/usr/local/lib/pkgconfig/`.

   **libaom is intentionally NOT enabled in this ADR.** The fork's
   `ffmpeg-patches/0007-libvmaf-tune-qpfile-unified.patch` carries
   a `qpfile_aom_apply_roi` helper that references `aom_roi_map_t`
   fields (`enabled`, `skip`, `ref_frame`, `delta_qp_enabled`)
   that do NOT exist in any released libaom version including
   v3.11.0 (verified 2026-05-18 against upstream
   `aom/aomcx.h:1609` — the public struct only has `roi_map`,
   `rows`, `cols`, `delta_q`, `delta_lf`, `static_threshold`).
   The patch was likely written against a libaom development
   branch whose ROI API never shipped. Enabling `--enable-libaom`
   therefore hard-errors at FFmpeg compile time. SVT-AV1 covers
   the production AV1 lane; libaom-av1 is the slow reference
   rarely used in production pipelines. Re-enabling libaom
   requires first fixing patch 0007 to either target a libaom
   version that ships the assumed ROI fields or make the ROI
   bridge libaom-version-gated (mirror the SVT-AV1 `>= 1.6.0`
   log-and-continue pattern). Tracked as a follow-up; out of
   ADR-0541's scope.
3. **VVenC built from source.** Not in Ubuntu apt. Cloned from
   `https://github.com/fraunhoferhhi/vvenc.git` at tag `v1.12.0`,
   built with CMake (`-DBUILD_SHARED_LIBS=ON
   -DVVENC_ENABLE_INSTALL=ON -DCMAKE_BUILD_TYPE=Release`),
   installed under `/usr/local`, `ldconfig` refresh, source tree
   deleted to keep image small.
4. **AMD AMF headers vendored.** AMF is header-only at FFmpeg build
   time. Cloned from `https://github.com/GPUOpen-LibrariesAndSDKs/
   AMF.git` at tag `v1.4.36`, only `amf/public/include/` copied to
   `/usr/local/include/AMF/` (1.5 MB). The runtime
   `libamfrt64.so` from amdgpu-pro is **not** baked in — see
   Consequences for the runtime contract.
5. **FFmpeg configure flags added**:
   `--enable-libaom --enable-libsvtav1 --enable-libvvenc
    --enable-nvenc --enable-cuda-nvcc
    --enable-libvpl --enable-amf`.
   `nv-codec-headers` was already installed in stage 3 for the
   `--enable-libvmaf-cuda` patch. No new layers needed for NVIDIA.
   `--enable-libnpp` is intentionally OMITTED: FFmpeg n8.1.1's NPP
   ceiling is CUDA 12.x, but the image's `cuda-toolkit` apt meta-
   package tracks CUDA 13.x, so passing the flag hard-errors at
   configure time with `ERROR: libnpp support is deprecated, version
   13.0 and up are not supported`. `scale_cuda` (via
   `--enable-cuda-nvcc`) covers the GPU hwupload → scale → nvenc
   pipeline without libnpp; `scale_npp` is the only filter we lose
   and it has no in-tree consumer. Documented inline in the
   Containerfile.
6. **Build-time encoder probe** added after `make install`. Iterates
   the 15 promised encoders (`libx264 libx265 libvpx-vp9 libaom-av1
   libsvtav1 libvvenc h264_nvenc hevc_nvenc av1_nvenc h264_qsv
   hevc_qsv av1_qsv h264_amf hevc_amf av1_amf`) and grep-matches each
   against the actual `ffmpeg -encoders` output. Mirrors the
   ADR-0514 libvmaf backend probe pattern — soft-fail with WARN
   rather than fail-build because hardware encoders may need
   userspace drivers absent from the BuildKit sandbox. The signal
   we lock down is the *compile-in* promise; a future configure
   tweak that silently drops `--enable-amf` prints `WARN h264_amf
   missing` on every rebuild.

`dev/AGENTS.md` documents the four new invariants so future rebases
preserve them. `docs/development/dev-mcp.md` updates the encoder
matrix and reproducer command.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(Chosen)** Distro packages for libsvtav1/libaom/libvpl + source build for libvvenc/AMF | Smallest delta to Containerfile; Ubuntu 24.04 SVT-AV1 1.7.0 already matches what BBB v10 sweeps assume; layer cache stays warm for the apt step on subsequent builds | Two source builds (vvenc + amf-headers); image grows ~80 MB | Best balance — only build from source what apt cannot supply. |
| Build all encoders from source (svtav1, aom, vvenc, amf) | Single update path (bump tags in one place); reproducible pin to upstream versions even if Ubuntu doesn't update | +25-40 min build time per rebuild; SVT-AV1 from source needs nasm + meson invocations that mirror what `libsvtav1-dev` already supplies; layer cache misses on every codec-tag bump | Disproportionate build-time cost for codec versions Ubuntu already tracks. |
| Use distro packages for everything (no source builds) | Fastest build; no upstream-tag tracking | `libvvenc-dev` does not exist in Ubuntu 24.04 apt; AMF is not packaged anywhere | Not feasible — the codecs we need most (VVenC + AMF) are not in apt. |
| Replace `libvpl` with legacy `libmfx` | Wider hardware compatibility (older Intel iGPUs) | Intel has sunsetted libmfx for Gen11+ silicon (the Arc A380 falls under this); FFmpeg upstream pivoted to libvpl in 7.x | libvpl is the supported successor and matches the host's Arc generation. |
| Add `--enable-vulkan` for the experimental Vulkan-Video encode path | Forward-looking; vendor-neutral hardware encode | FFmpeg 8.1 Vulkan-Video encoder is experimental, only AV1+H.265, and requires shaderInt64 + storage-buffer8bit extensions the host's gfx1036 doesn't expose | Premature; revisit when FFmpeg promotes the path out of experimental and the host AMD GPU gets driver support. |
| Defer hardware encoders to a separate PR | Smaller PR diff | Defeats the user's stated goal of unblocking the BBB v10 sweep across all 3 vendors; would need ADR-0541 *and* a follow-up ADR | Lumping into one PR is what the user explicitly requested ("LARGER PR"). |
| Pin nv-codec-headers to a newer ref to pick up AV1 NVENC | Required for `av1_nvenc` to appear in `-encoders` listing | Stage 3 already pins nv-codec-headers to `876af32a` (post-`cuStreamCreateWithPriority`) which is recent enough to include the av1_nvenc bindings | Already covered by existing pin — no action needed. |

## Consequences

- **Positive**: `vmaf-tune compare` sweeps can include the full
  software + hardware encoder matrix (15 encoders) on the dev host
  without any "not compiled into ffmpeg" skips. The BBB v10 sweep
  now produces ≥6 codec rows in the typical case. Cross-vendor
  rate-quality comparisons (NVENC vs QSV vs AMF AV1) become
  trivially reproducible inside the container.
- **Positive**: The build-time encoder probe surfaces silent
  configure-flag drift on every rebuild, matching the ADR-0514
  pattern. Future agents that "tidy up" the configure line cannot
  silently disappear an encoder.
- **Negative**: First-build wall time grows by ~5-10 minutes
  (libvvenc cmake build ~3 min; libaom + libsvtav1 are apt installs;
  AMF is a `cp -r` of headers). Image size grows ~80 MB (libvvenc.so
  libaom + libsvtav1 + AMF headers).
- **Negative — runtime hardware-encoder gating**: NVENC requires the
  NVIDIA Container Toolkit + a host driver with NVENC capability.
  AMF requires the amdgpu-pro userspace driver (`libamfrt64.so`) bind-
  mounted into the container — the standard open-source `amdgpu` /
  Mesa stack does **not** provide it. QSV requires the Intel media-
  driver and `/dev/dri/renderD*` passthrough. The container compiles
  these in, but the encoders' *runtime* availability depends on host
  state. vmaf-tune `compare.py::probe_encoder_available` already
  handles this: the encoder is in the listing (passes stage 1) but
  the 1-frame `lavfi nullsrc` dummy encode fails (stage 2), producing
  a stable `hardware encoder not available: <encoder> dummy encode
  failed: <reason>` skip. The skip is a per-encoder row, not a
  whole-sweep abort. The matrix in `docs/development/dev-mcp.md`
  documents which runtime drivers are required for each encoder.
- **Neutral**: `libnpp` is pulled in transitively by `cuda-toolkit`
  (already installed). No new CUDA layer.
- **Follow-up**: AMF runtime gap. To make `*_amf` encoders actually
  run inside the container on AMD-pro hosts, operators must bind-mount
  `/opt/amdgpu-pro/lib/x86_64-linux-gnu/libamfrt64.so` (the
  proprietary userspace) into the container. The open-source ROCm
  installation already in the image (`rocm-hip-runtime-dev`) does
  not include AMF. Documented as a known limitation in
  `docs/development/dev-mcp.md`. Not blocking — the encoders compile
  in and probe cleanly; runtime availability is a host config knob.

## References

- ADR-0496: `0496-prefer-dev-mcp-container-rule.md` — CLAUDE.md §12
  rule 15 default-to-container.
- ADR-0514: `0514-dev-container-full-backend-exposure.md` — sibling
  decision that unlocked GPU backend kernel dispatch; this ADR is
  the codec-side counterpart for FFmpeg encoders.
- ADR-0516: `0516-vmaf-tune-compare-rate-quality-sweep.md` — the
  `compare` subcommand whose encoder probe drove the discovery.
- ADR-0522: `0522-tiny-codec-preset-crf-cli-flags.md` — tiny-AI
  codec-aware inference which references the same encoder vocabulary.
- Source: `req` — user-provided briefing: "Build the `vmaf-dev-mcp`
  container's FFmpeg with libsvtav1 + libaom-av1 + libvvenc + the
  hardware encoder bindings (nvenc/qsv/amf). Currently the
  container's FFmpeg ships with only libx264, libx265, libvpx-vp9
  — which is why the BBB v10 compare agent could only sweep 3
  codecs."
