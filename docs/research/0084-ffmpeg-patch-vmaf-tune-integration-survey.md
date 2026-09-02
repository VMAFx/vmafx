<!-- markdownlint-disable MD033 MD056 MD060 -->
# Research-0084: FFmpeg-side integration patterns for video-quality tools (vmaf-tune)

- **Date**: 2026-05-05
- **ADR**: [0312](../adr/0312-ffmpeg-patches-vmaf-tune-integration.md)
- **Author**: Lusoris

## Objective

Understand how other video-quality assessment (VQA) tools integrate
with FFmpeg, and how the three major AV1/H.264 encoders expose
per-region or per-block QP offsets, so we can size the
`vmaf-tune` integration patch series correctly.

## Survey: VQA tooling × FFmpeg integration patterns

| Tool                | FFmpeg integration                                                          | Strategy                                                                                                                                      |
|---------------------|------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| **libplacebo**      | Filter graph (`vf_libplacebo`) — in-process; no encoder coupling.            | Stateful filter context, per-frame `pl_render_image`; AVOptions surface every `pl_render_params` field. We can mirror the AVOption density.   |
| **BUTTERAUGLI** (Google) | None upstream; Python harness shell-runs `butteraugli ref dist`.       | Same shape as our pre-vmaf-tune state: subprocess boundary, no in-FFmpeg score loop. Confirms the `vf_libvmaf_tune` scaffold is the upgrade path.|
| **SSIMULACRA 2**    | Two paths: in-libvmaf as a feature extractor (works through stock `libvmaf` filter); standalone `ssimulacra2` binary. | Reusing libvmaf's `feature=name=ssimulacra2` plumbing means **no new patch needed**; this fork already documents that. The same lesson applies to any new metric: ride the existing `feature=` machinery before adding a filter. |
| **VMAF**            | Stock upstream `libvmaf` filter + this fork's `libvmaf_cuda`/`_sycl`/`_vulkan` patches. | The fork's tiny-AI surface (`vmaf_dnn_*`) needed a patch (0001/0002) because it's not a feature-extractor; same posture for `vf_libvmaf_tune` — the recommend loop is not a libvmaf "feature".|
| **VEGA / DOVER / FUNQUE** | Python-only.                                                            | None integrates encoder-side ROI; the saliency-driven QP-offset pattern is novel and the qpfile parser is therefore fork-specific.            |

**Takeaway**: `-vf libvmaf_tune` should look like a tiny `vf_libvmaf.c`
sibling, not a new filter framework.

## Encoder-side QP / ROI ABI comparison

| Encoder       | API                                                              | Granularity         | Format on disk                                                | Plumbing in FFmpeg n8.1                                                                              |
|---------------|------------------------------------------------------------------|---------------------|---------------------------------------------------------------|------------------------------------------------------------------------------------------------------|
| **libx264**   | `x264_param_t::psz_qpfile` (per-MB, per-frame deltas, since r2390) | 16×16 luma MB       | ASCII; `<frame_idx> <I|P|B> <qp>` then per-MB-row deltas      | Already wired through `x264_param_parse(... "qpfile", path)` via `-x264-params qpfile=…`             |
| **libsvtav1** | `EbSvtAv1EncConfiguration` `region_of_interest_csv_path` (since SVT-AV1 1.5) | 64×64 superblock    | CSV: `<frame_idx>,<sb_x>,<sb_y>,<delta>`                      | **No plumbing today**: must be set programmatically before `svt_av1_enc_set_parameter`.              |
| **libaom-av1**| `aom_codec_control(AV1E_SET_ROI_MAP, aom_roi_map_t*)`            | 4-segment quantizer | In-memory `aom_roi_map_t` (no on-disk format)                 | **No plumbing today**: must build the roi_map struct per-frame.                                       |
| **libx265**   | `x265_param::rc.qpFile` (similar to x264)                        | 16×16 luma MB       | x264-compatible ASCII                                         | Wired through `-x265-params qpfile=…`. Out of scope for this PR but inherits the same parser.        |
| **libvpx**    | `VP9E_SET_ROI_MAP` (similar to libaom)                           | 4-segment quantizer | In-memory                                                     | Out of scope.                                                                                         |

**Takeaways**:

1. The qpfile **format** that vmaf-tune emits (a per-frame ASCII record
   followed by per-MB-row deltas) is a strict superset of x264's
   native qpfile, which is why libx264 needs zero new parser logic
   in the patch — just an option-name forward.
2. SVT-AV1 and libaom-av1 have very different ROI ABIs. A "true" ROI
   bridge for either is roughly 200–400 LOC and a day of testing —
   too much for one patch series. We ship the **shared parser** in
   patch 0007 so the bridges can land later as one-liner additions
   on each adapter, without re-deriving the qpfile format.
3. x265 inherits the parser for free if we ever decide to add a
   patch for it.

## Filter framework patterns relevant to `vf_libvmaf_tune`

The `vf_libvmaf.c` and `vf_vmaf_pre.c` filters in this patch series
already establish the conventions we need to replicate:

- 2-input filter via `FFFrameSync` (`vf_libvmaf.c` lines 203–625);
  patch 0008 reuses the dual-input init/activate idiom verbatim.
- AVOption table indexed off the priv struct (`OFFSET(field)`).
- Final-line emission at `uninit()` instead of side-data — matches
  upstream `vf_libvmaf.c`'s log-format dispatch but is simpler.
- `query_formats` enumerates a small pix_fmt list; `vf_libvmaf_tune`
  copies the YUV420/422/444 8/10-bit set since that's what vmaf-tune
  encodes against.

## Cost / LOC summary

| Patch | LOC added (excluding new files)                       | New files |
|-------|--------------------------------------------------------|-----------|
| 0007  | ~62 LOC across 4 upstream files + 2 new files (parser) | 2 |
| 0008  | ~14 LOC across 3 upstream files + 1 new filter         | 1 |
| 0009  | ~21 LOC in 1 upstream file                             | 0 |
| Total | ~97 LOC + 3 new files (~600 LOC across new files)      | 3 |

Well under the 2000-LOC patch budget agreed for this PR.

## References

- x264 qpfile docs: <https://www.videolan.org/developers/x264.html> (`--qpfile`)
- SVT-AV1 ROI CSV PR: AOMediaCodec/SVT-AV1#1843
- libaom ROI map header: `aom/aomcx.h` `AV1E_SET_ROI_MAP`
- ADR-0247 (vmaf-roi sidecar) — companion fork-side ROI work
- ADR-0286 (saliency_student_v1) — model that emits the qpfile
