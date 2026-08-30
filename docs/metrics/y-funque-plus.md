# Y-FUNQUE+ atoms

Y-FUNQUE+ is the single-channel (luma) member of the **FUNQUE+** full-reference
video-quality suite (Venkataramanan, Stejerean, Katsavounidis & Bovik, *One
Transform To Compute Them All*, arXiv:2304.03412; building on FUNQUE,
arXiv:2202.11241). It computes three perceptually-weighted **wavelet-domain
atom features** that the official model fuses — via a trained regressor — into a
single MOS-predicting score.

The fork ships the **atoms only**. The extractor `y_funque_plus` emits the three
atoms as separate metrics; it does **not** emit a fused Y-FUNQUE+ score. The
official `funque_plus` implementation ships no frozen regressor (it trains a
per-dataset SVR at runtime), so a fused number would be fork-originated and
needs a licensed subjective dataset plus a model card — that is a deferred
follow-up. See [ADR-1114](../adr/1114-y-funque-plus-atoms.md).

## Output

| Field | Value |
| --- | --- |
| Feature name | `y_funque_plus` |
| Output metrics | `y_funque_plus_ms_ssim`, `y_funque_plus_dlm`, `y_funque_plus_mad` |
| `y_funque_plus_ms_ssim` | MS-SSIM with covariance pooling. `0` when reference == distorted; grows with structural divergence. |
| `y_funque_plus_dlm` | DLM detail-loss measure. `≈ 1` when reference == distorted; **lower** = more detail lost. |
| `y_funque_plus_mad` | MAD-Ref temporal atom: mean abs difference of the reference's final approx subband between consecutive frames. `0` on frame 0. |
| Reference frame | Required (full-reference metric) |
| Backend | CPU (scalar) only |

There is **no** single `y_funque_plus` scalar — request the three atom keys
from the report. The atoms are not individually MOS-calibrated; they are the
inputs a fused Y-FUNQUE+ regressor would consume.

## Usage

```bash
vmaf \
    --reference ref.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --feature y_funque_plus \
    --output score.json
```

The three atom keys appear per-frame in the report and, pooled, under the same
keys in `pooled_metrics`.

## Algorithm

Per frame, on the luma plane only (chroma ignored), all in double precision:

1. **Normalize** — `y = pixel / ((1 << bpc) - 1)`, mapping luma to `[0, 1]`.
   SDR code-value domain (no EOTF).
2. **2x bicubic downscale** — OpenCV `INTER_CUBIC`: the Keys cubic kernel with
   `a = -0.75`, source coordinate `2i + 0.5`, `BORDER_REPLICATE` edges. This is
   the dominant cross-host parity component and is ported bit-faithfully.
3. **Crop** — to a multiple of `2^levels` computed from the **original**
   pre-resize dimensions: `w_crop = (orig_w >> (levels+1)) << levels`
   (`levels = 2`, so the cropped extent is a multiple of 4).
4. **2-level Haar DWT** — pywt `'periodization'` convention. Each level
   produces an approx subband and three detail subbands (H, V, D). The pyramid
   is `approxs = [A1, A2]`, `details = [(H1,V1,D1), (H2,V2,D2)]`.
5. **Nadenau CSF weighting** — multiply the **detail** subbands by the Nadenau
   Y-channel CSF weight for their (level, orientation). The **approx** subbands
   are never weighted. The analytic form regenerates the official lookup table
   to 8 dp.
6. **MS-SSIM atom (cov pooling)** — per scale, the luminance term comes from the
   approx subband and the contrast/structure terms from the detail-subband
   energy; the per-pixel SSIM map is pooled as `std(map) / mean(map)`; scales
   combine with exponents `[0.0448, 0.2856]` (sign-preserving power). `C1 =
   1e-4`, `C2 = 9e-4`.
7. **DLM atom (scale 2)** — on the last detail level: decouple the distortion
   into restored + additive parts (psi-angle mask `< 1°`, `k =
   clip(dis/(ref+eps), 0, 1)`), apply a 3×3 contrast mask (`/30`), then pool
   each subband's masked energy with cube-root pooling after a `0.2`-border
   crop. `dlm = (num + 1e-4) / (den + 1e-4)`. The numerator pools `rest^3`
   **without** abs while the denominator pools the reference detail **with**
   abs — an upstream asymmetry that is reproduced exactly.
8. **MAD-Ref atom (scale 2)** — `mean(|A2_ref[t] - A2_ref[t-1]|)`; `0` on the
   first frame. This makes the extractor temporal.

## Inputs and backends

| Property | Support |
| --- | --- |
| Backend | CPU (scalar) only |
| Pixel formats | Any planar YUV (luma plane only; chroma ignored) |
| Bit depth | 8 / 10 / 12 / 16-bit (luma normalized by `(1<<bpc)-1`) |
| Minimum frame size | Cropped extent must be ≥ 4 px in each dimension after the 2x downscale (very small frames are rejected with `-EINVAL`) |
| Temporal | Yes — the MAD-Ref atom caches the previous frame's final approx subband |

## Limitations

- **Atoms only, no fused score**: consumers get three atoms, not a single
  Y-FUNQUE+ MOS number. The fused ScaledSVR is deferred (needs a licensed
  subjective dataset + a model card). See
  [`docs/state.md`](../state.md) for the tracking row.
- **SDR code-value domain**: the metric normalizes raw luma with no
  transfer-function awareness. For PQ/HLG HDR content the perceptual CSF
  weighting is not calibrated; the metric still produces numbers but they are
  not HDR-calibrated (a Cut-FUNQUE/HDR-FUNQUE PU21 pre-step would be a separate
  variant).
- **Cross-host parity**: the OpenCV `INTER_CUBIC` downscale and the pywt
  `'periodization'` wrap are the dominant deviation sources. The extractor
  compiles with `-ffp-contract=off` and uses double precision throughout to
  keep the atoms inside the fork's places=4 atom gate.
- **No GPU / SIMD twins**: there is no CUDA/SYCL/HIP/Metal or AVX/NEON path.
  Future twins would not be bit-exact to CPU (cube-root and `exp` differ ~1 ulp
  across hosts), consistent with the fork's GPU-parity posture (places = 4).

## Correctness test

`core/test/test_y_funque_plus.c` (run via
`meson test -C core/build-cpu test_y_funque_plus`) asserts:

- identical-input analytic oracles (`ms_ssim = 0`, `dlm = 1`, `mad = 0`) at
  8×8, odd 65×33, and the 100×100 crop path;
- a 64×64 non-trivial oracle (`ms_ssim ≈ 0.0733072`, `dlm ≈ 0.9972564`) and a
  2-frame MAD-Ref temporal oracle (`mad ≈ 0.1199987`) at places = 4, both
  independently re-derived against a faithful `pywt` + OpenCV reference;
- a too-small-frame `init()` rejection.

## See also

- [ADR-1114: Y-FUNQUE+ wavelet-domain atom features](../adr/1114-y-funque-plus-atoms.md)
- [Research-0108: feasibility + constants digest](../research/0108-y-funque-plus-atoms.md)
- [SSIMULACRA 2](ssimulacra2.md) — the other fork wavelet/perceptual metric
- [Features overview](features.md)
