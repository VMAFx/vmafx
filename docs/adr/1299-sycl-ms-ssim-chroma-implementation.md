# ADR-1299: Compute MS-SSIM chroma on SYCL

Instead of accepting `enable_chroma` and ignoring it.

- **Status**: Accepted
- **Date**: 2026-09-23
- **Deciders**: Lusoris
- **Tags**: `sycl`, `gpu`, `feature-extractor`, `ms-ssim`, `parity`
- **Supersedes**: [ADR-0526](0526-ms-ssim-sycl-enable-lcs-parity.md) in part — its
  `enable_chroma` reasoning only; the `enable_lcs` decision stands.

## Context

ADR-0526 added `enable_chroma` to the SYCL MS-SSIM twin and recorded that
*"the `enable_chroma` option is structural (MS-SSIM is luma-only by construction)
and is accepted for option-table symmetry without semantic effect."*

**That premise is false.** MS-SSIM is not luma-only by construction, and this
repository is the proof: `core/src/feature/float_ms_ssim.c` has looped
`n_planes` and emitted `float_ms_ssim_cb` / `float_ms_ssim_cr` since PR #939.
The option was therefore not "structural" — it was unimplemented, and the ADR
recorded the absence as a property of the metric.

The consequence was a silent one. `configure_ms_ssim` computed
`s->n_planes = enable_chroma ? 3 : 1` and nothing ever read `n_planes` —
the identifier occurred exactly twice in the translation unit, once as the field
and once in that assignment. Setting the option changed nothing, raised nothing,
and `provided_features` advertised only `float_ms_ssim`, so a model asking for
the chroma features had them served by the CPU twin under the ADR-0530
name-based fallback. Scores were right; the acceleration was absent; nothing
said so.

Two further facts surfaced while closing this:

1. **The CPU reference was broken for chroma below 352x352 luma.** `init()` in
   `float_ms_ssim.c` checked the 5-level pyramid minimum against luma only, so a
   4:2:0 input between `min_dim` and `2 * min_dim` passed and then died mid-run
   inside upstream `ms_ssim.c`, which prints `error: scale below 1x1!` to stdout
   and returns 1. Reproduced on this repository's own primary Netflix fixture:
   `--width 576 --height 324 --pixel_format 420 --feature
   float_ms_ssim=enable_chroma=true` emitted that line, logged a bare "problem
   with feature extractor", and wrote no output file at all. 576x324 gives
   288x162 chroma, and 162 < 176.

2. `docs/metrics/ms-ssim.md` claimed the SYCL twin implemented chroma "fully
   (3 planes)". PR #1520 corrected that text to "accepted but a no-op" and filed
   a tracking row. Correcting the sentence was right; stopping there was not.
   A missing backend leg is not a documentation defect.

## Decision

Implement it.

- `MsSsimPlaneGeometry` holds the per-plane dimensions, staging buffers and
  pyramid. Geometry is per plane because a 4:2:0 chroma plane is a different
  size from luma and every kernel takes those dimensions as a row pitch;
  feeding a chroma plane through luma geometry reads at the wrong pitch, which
  is a wrong number and an out-of-bounds read rather than a crash.
- The reduction workspace stays shared and plane-0 sized. No chroma plane is
  wider or taller than luma in any supported pixel format
  (`picture.c:146-149`), so plane-0 sizing dominates. Planes run sequentially
  for the same reason the five scales already do: the horizontal intermediates
  and the partials are one workspace, and `compute_scale_lcs` waits on its
  readback before returning.
- Every plane is staged in `submit_fex_sycl`, not `collect_fex_sycl`, because
  libvmaf's double-buffered GPU dispatch releases the `VmafPicture` once submit
  returns.
- `provided_features` advertises `float_ms_ssim_cb` and `float_ms_ssim_cr`, so
  the features are served by the GPU twin rather than silently by the CPU one.
- The per-scale `l`/`c`/`s` breakdown stays luma-only, matching the CPU twin's
  `p == 0` guard.
- Both twins now reject an input whose **chroma** planes fall under the pyramid
  minimum, naming the actual chroma size and the luma resolution that would
  satisfy it.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Implement chroma on SYCL** (chosen) | The option now does what it says, and the feature is accelerated rather than quietly served by the CPU. |
| Keep the no-op, document it accurately | What PR #1520 did. It makes the record honest and leaves the backend leg missing; a user enabling chroma on SYCL still gets no GPU chroma. |
| Reject `enable_chroma` on SYCL, as CUDA does | Honest and cheap — CUDA has no such option and errors with `-EINVAL`. But it removes a capability the hardware can serve, and the CPU twin already defines the semantics. |
| Give every GPU twin the option and implement none | The status quo across HIP and Metal. It is the shape that produced this defect. |

## Consequences

- **Positive**: `enable_chroma=true` on SYCL computes chroma on the GPU.
  Verified on an Intel Arc A380 against the CPU twin with a textured-chroma
  4:2:0 fixture: `float_ms_ssim_cb` 0.946694, `float_ms_ssim_cr` 0.986886,
  identical to the CPU to all six emitted digits across three frames, with GPU
  time rising 15.32 ms to 18.57 ms per frame-pair as the extra planes are
  dispatched.
- **Positive**: chroma MS-SSIM no longer dies mid-run on sub-352x352 4:2:0
  input on either twin; it is refused at init with the resolution it needs.
- **Negative**: three planes cost roughly 21% more GPU time when enabled.
  Default is unchanged (`enable_chroma=false`).
- **Negative**: `n_dispatches_per_frame` on `VmafFeatureCharacteristics` is a
  static field and cannot vary with an option, so the scheduler's cost model
  still reflects the luma-only dispatch count. Recorded rather than worked
  around.
- **Open**: HIP (`integer_ms_ssim_hip.c` assigns `n_planes = 1u` on both arms of
  its `if`), CUDA (no `enable_chroma` at all) and Metal keep the same gap. This
  ADR closes SYCL only; the rest are tracked in `docs/state.md`.

## References

- req: "so a missing leg or gap is a docfix for you?" — the user, on PR #1520.
- [ADR-0526](0526-ms-ssim-sycl-enable-lcs-parity.md) — the superseded premise.
- [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md) —
  the name-based fallback that made the missing leg invisible.
- [ADR-0153](0153-float-ms-ssim-min-dim-netflix-1414.md) — the luma pyramid
  minimum, which this extends to every scored plane.
