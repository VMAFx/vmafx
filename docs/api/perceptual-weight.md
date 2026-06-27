<!-- markdownlint-disable MD013 -->
# Perceptual spatial-pooling weighting — `libvmaf/perceptual_weight.h`

This C API lets libvmaf consume the per-frame side-data that the
[Pelorus](https://github.com/VMAFx/pelorus) GPU pre-encode pipeline attaches to
each distorted frame, and use it to **perceptually re-weight VMAF's spatial
pooling**: frames whose spatial regions carry high banding risk count for more
in a pooled score. It builds on the vendored
[Pelorus interop ABI](pelorus-interop.md) (ADR-1113); the design and the
golden-gate guarantee are recorded in
[ADR-1118](../adr/1118-perceptual-sidedata-weighting.md).

> **Golden-gate isolation — the #1 invariant.** The weighting is **inert** unless
> **both** (a) it is enabled (`vmaf_set_perceptual_weight_enabled`, default OFF)
> **and** (b) a valid Pelorus side-data blob is registered for the frame
> (`vmaf_set_perceptual_sidedata`). When either is false, the per-frame weight is
> exactly `1.0` and pooling is **byte-identical** to a build without this
> feature. The Netflix golden reference pairs carry no Pelorus side-data, so they
> score bit-exact whether or not the option is on. Do not change this.

## When to use it

Use it when you score a chain that includes Pelorus pre-encode filters
(`vf_pelorus_deband` / `vf_pelorus_analyze`) and you want a pooled VMAF that
tracks the distortions a deband-aware encoder cares about — not a plain mean over
frames. If you are not running Pelorus, or you want a vanilla pooled VMAF, leave
it off (the default) and nothing changes.

## API

```c
#include <libvmaf/perceptual_weight.h>
```

### `vmaf_set_perceptual_weight_enabled`

```c
int vmaf_set_perceptual_weight_enabled(VmafContext *vmaf, int enabled);
```

Master opt-in switch, **OFF by default**. While disabled, any registered
side-data is retained but never consulted and pooling is byte-identical to
upstream. Toggling at runtime affects subsequent pooling calls only; it never
mutates an already-computed per-frame VMAF score. Returns `0`, or `-EINVAL` when
`vmaf` is `NULL`.

### `vmaf_set_perceptual_weight_strength`

```c
int vmaf_set_perceptual_weight_strength(VmafContext *vmaf, double strength);
```

Sets the weighting strength. The per-frame weight is `1 + strength · salience`,
where `salience` is the normalized `[0,1]` banding/variance activity derived from
the frame's side-data. The default is `1.0`; `strength == 0` makes the weighting
an identity even when enabled. Returns `0`, or `-EINVAL` when `vmaf` is `NULL` or
`strength` is negative / not finite.

### `vmaf_set_perceptual_sidedata`

```c
int vmaf_set_perceptual_sidedata(VmafContext *vmaf, const uint8_t *blob,
                                 size_t len, unsigned pic_index);
```

Registers a Pelorus side-data blob for a picture index. The blob is the full
`AV_FRAME_DATA_SEI_UNREGISTERED` payload (16-byte UUID prefix + flat
`PelorusSideData` image). The parser extracts the banding/variance summaries plus
per-cell maps and stores a derived per-frame salience keyed by `pic_index`. The
blob is **not retained** — only the derived summary is copied — so the caller may
free `blob` immediately after the call.

| Return | Meaning |
| --- | --- |
| `0` | Summary stored. |
| `-ENOENT` | `blob` is not a Pelorus blob (wrong UUID/magic). Cleanly ignored — the frame stays unweighted. |
| `-EPROTO` | ABI-major mismatch. Cleanly ignored and logged — the frame stays unweighted (interop rule R6). |
| `-EINVAL` | `vmaf` or `blob` is `NULL`. |
| `-ENOMEM` | Allocation failure. |

This is a no-op store with no effect on scoring unless weighting is also enabled.

## How the weight is derived

For each frame with a valid blob:

1. **Banding** is the primary driver. When the blob carries a non-empty cell grid
   and a banding cell map, the salience is the mean per-cell banding risk
   (`uint8` cells in `[0,255]` → `[0,1]`). When the grid is empty
   (`grid_cols == 0`, today's `vf_pelorus_deband` placeholder) or the map is
   absent/truncated, it falls back to the frame-level scalars
   (`global_banding_risk` blended with `flat_area_fraction`).
2. **Variance** modulates it. Low spatial variance (flat regions) makes banding
   more visible, so it lifts the salience slightly; high variance (texture) masks
   it. The modulation factor stays in `[0.75, 1.25]` so banding dominates.
3. **Complexity** attenuates it (Pelorus ABI ≥ 1.3, `PEL_SEC_COMPLEXITY`). The
   producer's aggregate per-frame complexity scalar `[0,1]` scales the salience
   by `(1 − 0.5 · complexity)`, floored at `0.25`: banding and compression
   artefacts are most visible on flat / simple frames and masked on busy /
   textured ones, so a maximally-complex frame keeps only half its banding
   salience while a flat frame (`complexity == 0`) is unchanged. This section is
   per-frame (grid-independent), so it also modulates the `grid == 0` scalar
   path. When the producer attaches **no** complexity section (any older
   producer, or the Netflix golden pairs) the factor is exactly `1.0` and the
   weight is byte-identical to the legacy banding+variance derivation.
4. The salience is clamped to `[0,1]`; the weight is `1 + strength · salience`,
   always finite and positive.

The pooled score then becomes:

- **MEAN** → weighted mean `Σ(wᵢ·sᵢ) / Σ(wᵢ)`.
- **HARMONIC_MEAN** → weighted harmonic mean `Σwᵢ / Σ(wᵢ/(sᵢ+1)) − 1`.
- **MIN / MAX** → unchanged (re-weighting cannot reorder the extremes).

With every `wᵢ = 1` (no side-data, or weighting off) these reduce to the exact
upstream formulas, and the implementation runs the *literal* upstream expression
on that path, so the result is byte-identical, not merely numerically close.

## Forward / back-compat (interop rules R1–R6)

The reader is robust to an evolving Pelorus producer:

- **R4** — each section is read for `min(known_size, dir.size)` bytes; a newer
  producer's appended fields are ignored.
- **R3** — unknown section bits are ignored.
- Absent sections, or a blob with `grid == 0`, degrade to a frame-level scalar
  (or to "no salience", weight `1.0`) — never a crash.
- **R6** — an ABI-major mismatch is rejected (`-EPROTO`), the frame is scored
  unweighted, and a warning is logged once.

All per-cell map reads are bounds-checked against the blob image before any
dereference; NaN/Inf inputs are clamped.

## FFmpeg integration

The `vf_libvmaf` filter exposes a boolean `perceptual_weight` AVOption
(**default 0 = OFF**). When set, the filter reads any Pelorus blob off the
distorted frame (`av_frame_get_side_data(..., AV_FRAME_DATA_SEI_UNREGISTERED)`)
and drives this API per frame. The wiring ships as
[`ffmpeg-patches/0017-libvmaf-read-pelorus-sidedata.patch`](../../ffmpeg-patches/0017-libvmaf-read-pelorus-sidedata.patch).

```text
ffmpeg -i ref.mp4 -i dist.mp4 \
  -lavfi "[0:v][1:v]libvmaf=perceptual_weight=1" -f null -
```

With `perceptual_weight=0` (the default) the filter behaves exactly like upstream.
