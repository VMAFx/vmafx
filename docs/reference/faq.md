<!-- markdownlint-disable MD013 -->
# Frequently Asked Questions

> This FAQ covers both the upstream [Netflix/vmaf](https://github.com/Netflix/vmaf)
> Q&A and fork-specific entries for the SYCL / CUDA / HIP backends, SIMD paths,
> and the tiny-AI model surface. Upstream issue numbers (e.g. `Netflix/vmaf#20`)
> are kept for historical context — the issues themselves are long-resolved.

## Scoring & models

### Q: When computing VMAF on low-resolution videos (480-pixel height, for example), why do the scores look so high, even when there are visible artifacts?

A: VMAF embeds an implicit assumption about viewing distance and display size.

Any perceptual quality model has to account for viewing distance and display
size (or their ratio). The same distorted video, viewed close-up, contains more
visible artifacts and so has lower perceptual quality.

The default VMAF model (`model/vmaf_float_v0.6.1.pkl`) is trained to predict
the quality of videos displayed on a 1080p HDTV in a living-room environment.
All subjective data was collected with distorted videos rescaled to 1080 and
displayed from a viewing distance of three times the screen height (3H) — an
angular resolution of 60 pixels per degree. The implicit assumption is: *a
1080 video displayed from 3H away*.

When VMAF is calculated on a 480-resolution pair, it is as if the 480 video is
*cropped* from a 1080 video. If the 480 video has height H', then
H' = 480/1080 · H ≈ 0.44 · H. VMAF is then effectively modelling viewing
distance of 3H = 6.75 · H'. In other words, running VMAF on a native-480 pair
predicts the perceptual quality of viewing from 6.75× the screen height —
which hides a lot of artifacts and inflates the score.

One implication: **do not compare the absolute VMAF score of a 1080 video with
the score of a 480 video obtained at its native resolution** — it is apples vs
oranges.

To predict quality at 3× height for a 480 pair:

- If the 480 distorted video has a 1080 reference, upsample the 480 distorted
  to 1080 and run VMAF at 1080.
- If both are 480, upsample both to 1080 and run VMAF. The default model was
  not trained on upsampled references, so prediction is less accurate than
  the first option.

### Q: Will VMAF work on 4K videos?

A: The default model (`model/vmaf_v0.6.1.json`) was trained on videos encoded
at resolutions up to 1080p. It is still useful for 4K if you only need a
*relative* score (A vs B ordering), but absolute predictions are not
guaranteed.

For 4K-specific scoring, use the dedicated 4K model
`model/vmaf_4k_v0.6.1.json`, which predicts 4KTV viewing at 1.5× display
height. See the
[Predict Quality on a 4KTV Screen at 1.5H](../models/overview.md#predict-quality-on-a-4ktv-screen-at-15h)
section of the models document.

### Q: When I compare a video with itself as reference, I expect a perfect VMAF score of 100, but I see ~98.7. Is this a bug?

A: No. VMAF does not guarantee a perfect score on identical inputs, but should
return a value close to 100. The same is true of other ML-based predictors
(e.g. VQM-VFD). The gap comes from features (ADM, VIF) that do not have a
closed-form identity at the fit-polynomial output stage.

### Q: How is the VMAF package versioned?

A: The VMAF number in the `VERSION` file tracks the default model consumed by
`VmafQualityRunner`. Whenever the default model changes in a way that alters
the numerical output, the number is bumped. For `libvmaf` (the C library) and
for everything else, version numbers follow the package version in
`libvmaf.pc`.

> **Fork note:** the Lusoris fork uses `vX.Y.Z-lusoris.N` — upstream Netflix
> version + fork-specific revision. See the
> [release guide](../development/release.md).

### Q: Why is the aggregate VMAF score sometimes biased toward "easy" content? (upstream `Netflix/vmaf#20`)

A: The default aggregate is the arithmetic mean of per-frame scores — chosen
for simplicity and consistency with other metrics (e.g. mean PSNR).
Psycho-visual evidence suggests humans weigh the worst-quality frames more
heavily, so mean is not necessarily optimal.

Pooling can be changed via `--pool` on `vmaf` and via the `pool_method`
argument to `run_vmaf`, `run_psnr`, `run_vmaf_training`, `run_testing` and
friends. Accepted values: `mean`, `harmonic_mean`, `median`, `min`, `perc5`,
`perc10`, `perc20`.

## Inputs & formats

### Q: Can I pass encoded H.264 / VP9 / HEVC bitstreams to VMAF as input? (upstream `Netflix/vmaf#55`)

A: Yes — use FFmpeg's `libvmaf` filter. Details in the
[FFmpeg documentation](../usage/ffmpeg.md).

### Q: How do I use VMAF with downscaled videos?

A: If a distorted video was scaled down (e.g. for adaptive streaming) and you
want to evaluate its quality at the reference resolution, use FFmpeg with the
`libvmaf` filter to rescale on-the-fly.

For example, to upscale the distorted video to 1080p:

```bash
ffmpeg -i main.mpg -i ref.mpg \
  -filter_complex "[0:v]scale=1920x1080:flags=bicubic[main];[main][1:v]libvmaf" \
  -f null -
```

This scales the first input video (`0:v`) to 1080p and forwards it to VMAF
(`libvmaf`) as `main`, to be compared against the second input `1:v`. See the
[FFmpeg documentation](../usage/ffmpeg.md) for more.

### Q: Why does the included SSIM tool produce different numbers than other SSIM implementations?

A: The SSIM implementation in VMAF includes the empirical downsampling step
from the *Suggested Usage* section of
[SSIM](https://ece.uwaterloo.ca/~z70wang/research/ssim/). FFmpeg's `ssim`
filter, for example, does not include this step.

### Q: Why are PSNR values capped at 60 dB for 8-bit inputs, 72 dB for 10-bit, 84 dB for 12-bit, and 108 dB for 16-bit?

A: The caps follow the rule of thumb `6·N + 12`, where `N` is the bit depth.

This approximates the more precise
`10 · log10( (2^N - 1)^2 / (1/12) )`, where `2^N - 1` is the peak signal of an
N-bit representation and `1/12` is the variance of `Uniform[0, 1]`-distributed
quantisation noise (the noise of rounding a real-valued signal to its integer
representation). For `N = 8`, the precise formula yields 58.92 dB; the
rule-of-thumb rounds to 60.

It is true that an 8-bit distorted signal could match an 8-bit reference
exactly and produce infinite PSNR, but that only happens because the reference
is itself in 8-bit representation. In general, references come from higher-bit
or floating-point sources, and PSNR is bounded by the quantisation-noise floor
above. A bit-depth-based PSNR cap reflects that more realistic fidelity
ceiling.

## Training

### Q: If I train a model with `run_vmaf_training` and then test with `run_testing` on the same dataset, why don't I get the same SRCC / PCC / RMSE? (upstream `Netflix/vmaf#191`)

A: The two scripts use slightly different pipelines.

`run_vmaf_training` extracts per-frame feature scores, temporally pools each
feature (arithmetic mean) into a per-clip feature score, then fits the model
against subjective scores. The reported metrics are the fitting result.

`run_testing` extracts per-frame features, applies the prediction model
per-frame, and then arithmetic-means the per-frame VMAF scores into a clip
score. This is a *re-ordering* of "temporal pooling" and "prediction".

If features are constant across a clip, the order does not matter. In
practice, the difference is small but non-zero.

### Q: How do I train a custom VMAF model?

A: See the upstream
[Python library guide](../usage/python.md#train-a-new-model). For the fork's
tiny-AI models (ONNX-based quality predictors), see
[docs/ai/training.md](../ai/training.md) and the `ai/` package
(`pip install -e ai && vmaf-train --help`).

## Performance & backends (fork-specific)

### Q: Does the fork support GPU acceleration?

A: Yes. The fork adds CUDA and SYCL backends on top of upstream (HIP is
planned; see [backends/index.md](../backends/index.md)). Enable the
backends at build time:

```bash
cd libvmaf
meson setup build -Denable_cuda=true -Denable_sycl=true
ninja -C build
```

See [backends/](../backends/index.md) for per-backend notes and
[development/build-flags.md](../development/build-flags.md) for every
`meson_options.txt` option. When the binary is built with a GPU
backend it is **auto-selected at runtime** — there is no `--cuda` or
`--sycl` selector flag. Opt out via `--no_cuda` / `--no_sycl`, or pin
a specific SYCL device with `--sycl_device N`. The CPU path with
AVX2 / AVX-512 / NEON SIMD is the universal fallback.

### Q: How do I get bit-exact round-trippable VMAF output?

A: Pass `--precision=max` to the fork-added flag on the `vmaf` CLI. The
default format string is `%.6f` (Netflix-compatible per
[ADR-0119](../adr/0119-cli-precision-default-revert.md));
`--precision=max` (alias `full`) selects `%.17g` for IEEE-754
round-trip lossless output. `--precision=N` (1..17) overrides with
`"%.<N>g"`.

### Q: What are the "tiny-AI" models and how do I use them?

A: Tiny-AI is a fork-added surface for small ONNX perceptual quality models
that run through ONNX Runtime inside libvmaf. See
[docs/ai/overview.md](../ai/overview.md) for the architecture and
[docs/ai/inference.md](../ai/inference.md) for the CLI.

The fork now ships registry-pinned ONNX artefacts under
[`model/tiny/`](../../model/tiny/), including VMAF-tiny FR regressors,
codec-aware FR regressors, LPIPS-SqueezeNet, saliency students,
TransNet V2, and learned pre-filter models. Check
[`model/tiny/registry.json`](../../model/tiny/registry.json) for the
authoritative list and each row's `smoke` flag; smoke entries are CI or
compatibility probes, not production quality models. Invocation is:

```bash
vmaf --reference ref.y4m --distorted dis.y4m \
  --tiny-model model/tiny/vmaf_tiny_v2.onnx \
  --tiny-device auto
```

Production deployments should combine the model registry with
`--tiny-model-verify` and, where appropriate, the `VMAF_TINY_MODEL_DIR`
path jail documented in [docs/ai/security.md](../ai/security.md).

### Q: Does the fork preserve Netflix's golden-data numerical contract?

A: Yes, on the CPU path. The three canonical Netflix reference test
pairs (src01/hrc00–hrc01, checkerboard 1-pixel shift, checkerboard
10-pixel shift) run in CI as a required status check and the CPU
scalar + fixed-point path must match upstream exactly.

The **GPU backends (CUDA, SYCL) are not bit-exact** with the CPU path —
they never were, not even upstream. Different reduction orders,
parallel-prefix scans, and FMA contractions on GPUs introduce small
ULP-level deltas. The same is true at a smaller scale for the SIMD
paths (AVX2 / AVX-512 / NEON). Agreement between CPU and
CUDA/SYCL/SIMD is typically ~6 decimals on the pooled VMAF — enough
that the default (`%.6f`, alias `--precision=legacy`) hides the delta,
but `--precision=max` (`%.17g`) exposes it.

GPU / SIMD paths are regression-tested by fork-added snapshot JSONs
under `testdata/` (per-backend, ULP tolerance, regenerated via
`/regen-snapshots`), not by the Netflix goldens. See
[engineering principles §3.1](../principles.md#31-netflix-golden-data-gate)
for the scope of the CPU gate.

## Applications

### Q: Will VMAF work on applications other than HTTP adaptive streaming?

A: VMAF was designed with HTTP adaptive streaming in mind. It targets
compression artifacts and scaling artifacts (see Netflix's
[tech blog post](https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652)
for context). Artifacts outside that space — transmission errors, packet loss,
pre-capture noise, encoder-specific pathologies — may be predicted
inaccurately.
