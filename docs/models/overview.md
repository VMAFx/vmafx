<!-- markdownlint-disable MD013 -->
# Models

> **New: VMAF v1 (v1.0.16).** A newer generation of SDR models (ported from
> Netflix upstream `4718b4f5f`) is documented separately in
> [VMAF v1 (v1.0.16)](v1.md). This page covers the previous generation (v0).

This repository offers a number of pre-trained VMAF models to be used in different scenarios. Besides the default VMAF model which predicts the quality of a video displayed on a HDTV in a living-room viewing condition, this repository also includes a number of additional models, covering mobile phone and 4KTV viewing conditions.

## Predict Quality on a 1080p HDTV screen at 3H

The default VMAF model (`model/vmaf_v0.6.1.json`) is trained to predict the quality of videos displayed on a 1080p HDTV in a living-room-like environment. All the subjective data were collected in such a way that the distorted videos (with native resolutions of 1080p, 720p, 480p etc.) get rescaled to 1080 resolution and shown on the 1080p display with a viewing distance of three times the screen height (3H). Note that 3H is the critical distance for a viewer to appreciate 1080p resolution sharpness (see [recommendation](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2022-0-201208-W!!PDF-E.pdf)).

This model is trained using subjective data collected in a lab experiment, based on the [absolute categorical rating (ACR)](https://en.wikipedia.org/wiki/Absolute_Category_Rating) methodology, with the exception that after viewing a video sequence, a subject votes on a continuous scale (from "bad" to "excellent", with evenly spaced markers of "poor", "fair" and "good" in between), instead of the more conventional five-level discrete scale. The test content are video clips selected from the Netflix catalog, each 10 seconds long. For each clip, a combination of 6 resolutions and 3 encoding parameters are used to generate the processed video sequences, resulting 18 impairment conditions for testing.

The raw subjective scores collected are then cleaned up using the MLE methodology, as described in [SUREAL](https://github.com/Netflix/sureal). The aggregate subjective scores after clean-up are mapped to a score in the VMAF score, where "bad" is mapped to roughly score 20, and "excellent" to 100.

## Predict Quality on a Cellular Phone Screen

The default VMAF model (`model/vmaf_v0.6.1.json`) has a companion phone-screen mode, activated via `--phone-model`. From the C CLI:

```bash
vmaf --reference src01_hrc00_576x324.yuv \
     --distorted src01_hrc01_576x324.yuv \
     --width 576 --height 324 --pixel-format 420 --bitdepth 8 \
     --model version=vmaf_v0.6.1:phone_model=1
```

From the Python scripts:

```bash
python -m vmaf.script.run_vmaf yuv420p 576 324 \
    src01_hrc00_576x324.yuv \
    src01_hrc01_576x324.yuv \
    --phone-model
```

The subjective experiment uses similar video sequences as the default 1080p HDTV model, except that they were watched on a cellular phone screen (Samsung S5 with resolution 1920x1080). Instead of fixating the viewing distance, each subject is instructed to view the video at a distance he/she feels comfortable with. In the trained model, the score ranges from 0 to 100, which is linear with the subjective voting scale, where roughly "bad" is mapped to score 20, and "excellent" is mapped to score 100.

Invoking the phone model will generate VMAF scores higher than in the regular model, which is more suitable for laptop, TV, etc. viewing conditions. An example VMAF–bitrate relationship for the two models is shown below:

![regular vs phone model](/resource/images/phone_model.png)

From the figure it can be interpreted that due to the factors of screen size and viewing distance, the same distorted video would be perceived as having a higher quality when viewed on a phone screen than on a laptop/TV screen, and when the quality score reaches its maximum (100), further increasing the encoding bitrate would not result in any perceptual improvement in quality.

## Predict Quality on a 4KTV Screen at 1.5H

A 4K VMAF model at `model/vmaf_4k_v0.6.1.json` predicts the subjective quality of video displayed on a 4KTV and viewed from the distance of 1.5 times the height of the display device (1.5H). Again, this model is trained with subjective data collected in a lab experiment, using the ACR methodology (notice that it uses the original 5-level discrete scale instead of the continuous scale). The viewing distance of 1.5H is the critical distance for a human subject to appreciate the quality of 4K content (see [recommendation](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2022-0-201208-W!!PDF-E.pdf)). More details are in [this slide deck](../reference/presentations/VQEG_SAM_2018_025_VMAF_4K.pdf).

To invoke this model, specify the model path on the `vmaf` CLI:

```bash
vmaf --reference ref_path --distorted dis_path \
     --width 3840 --height 2160 --pixel-format 420 --bitdepth 8 \
     --model path=model/vmaf_4k_v0.6.1.json
```

Or from the Python scripts:

```bash
python -m vmaf.script.run_vmaf yuv420p 3840 2160 \
    ref_path dis_path \
    --model model/vmaf_4k_v0.6.1.json
```

## Disabling Enhancement Gain (NEG mode)

For comparing encoders, VMAF offers a special mode, called *No Enhancement Gain*. This is described in the [following blog post](https://netflixtechblog.com/toward-a-better-quality-metric-for-the-video-community-7ed94e752a30):

> One unique feature about VMAF that differentiates it from traditional metrics such as PSNR and SSIM is that VMAF can capture the visual gain from image enhancement operations, which aim to improve the subjective quality perceived by viewers. (…) However, in codec evaluation, it is often desirable to measure the gain achievable from compression without taking into account the gain from image enhancement during pre-processing.

To disable enhancement gain, use the versions of the model files ending with `neg`.

More details on the reasoning behind NEG have been shared in [this tech memo](https://docs.google.com/document/d/1dJczEhXO0MZjBSNyKmd3ARiCTdFVMNPBykH4_HMPoyY/edit#heading=h.oaikhnw46pw5). A high-level overview can be found in [this slide deck](https://docs.google.com/presentation/d/1ZVQPsA4N6K8uGW3aFgw4Ei9w953nYORUUPvgpigOq58/edit?usp=sharing).

## What are the Differences between Individual Models?

There are no material differences between 0.6.1, 0.6.2, and 0.6.3. The latter two were retrained later on the same dataset with the same hyperparameters, but using elementary features that were slightly improved.

The 0.6.2 and 0.6.3 models also come in `_b` (bootstrap) variants, which enable prediction confidence intervals. See the [confidence interval document](../metrics/confidence-interval.md).

Each model also has a corresponding `_float_` variant (e.g. `vmaf_float_v0.6.1.pkl` / `vmaf_float_v0.6.1.json`), which evaluates features in double-precision floating-point instead of the default fixed-point path. The fixed-point path is bit-exactly reproducible; the floating-point path is the one the tuning experiments were run against.

## GPU and SIMD acceleration (fork-specific)

All models above run unchanged on the fork's CUDA and SYCL backends (HIP is planned — see [backends/index.md](../backends/index.md)) and on AVX2 / AVX-512 / NEON SIMD on x86 and ARM CPUs. GPU backends are enabled at build time (`-Denable_cuda=true`, `-Denable_sycl=true`) and auto-selected at runtime; opt out per invocation via `--no_cuda` / `--no_sycl`. The CPU scalar + fixed-point path is the archival reference — it is the only backend the three Netflix golden-data checkpoints assert against (see [principles.md §3.1](../principles.md#31-netflix-golden-data-gate)). CUDA, SYCL, and SIMD paths produce scores that agree with the CPU path to ~6 decimal places but are not bit-exact — differing reduction orders and FMA contractions on parallel hardware always introduce small ULP-level deltas. Fork-added per-backend snapshot tests catch regressions within that tolerance.

## Tiny-AI models (fork-specific)

The fork also ships small ONNX Runtime models under
[`model/tiny/`](../../model/tiny/) for the tiny-AI surface documented in
[`docs/ai/`](../ai/). These are not replacements for the Netflix SVM
models above; they are opt-in augmentations and companion extractors for
full-reference regressors, no-reference heads, saliency maps, shot
boundaries, perceptual-distance features, and learned pre-filters.

The registry at
[`model/tiny/registry.json`](../../model/tiny/registry.json) is the
source of truth for shipped tiny-AI artefacts. Production entries include:

| Family | Examples | Use |
| --- | --- | --- |
| VMAF-tiny FR regressors | `vmaf_tiny_v2`, `vmaf_tiny_v3`, `vmaf_tiny_v4`, `fr_regressor_v1`, `fr_regressor_v2`, `fr_regressor_v3` | Estimate teacher VMAF from compact feature vectors; v2/v3/v4 are the progressive tiny VMAF ladder. |
| Probabilistic / codec-aware FR | `fr_regressor_v2_ensemble_v1_seed0..4` | Ensemble members for uncertainty-aware vmaf-tune decisions. |
| Perceptual features | `lpips_sq_v1` | Full-reference LPIPS-SqueezeNet perceptual distance. |
| No-reference / saliency | `nr_metric_v1`, `saliency_student_v1`, `saliency_student_v2`, `transnet_v2` | NR quality, saliency maps, and shot-boundary detection. |
| Learned filters | `learned_filter_v1`, `fastdvdnet_pre` | Pre-filter / denoising surfaces used before scoring or encoding. |

Smoke-only entries such as `smoke_v0`, `smoke_fp16_v0`, and historical
placeholder rows remain in the registry for CI and compatibility; check
the row's `smoke` flag and the per-model card before treating an entry
as production.

Use a tiny model through the `vmaf` CLI with `--tiny-model`:

```bash
vmaf --reference ref.y4m --distorted dis.y4m \
     --tiny-model model/tiny/vmaf_tiny_v2.onnx \
     --tiny-device auto
```

For runtime details, see [tiny-AI inference](../ai/inference.md). For the
registry schema, signatures, and model identity checks, see
[model-registry.md](../ai/model-registry.md). For path, size, operator,
and signature hardening, see [security.md](../ai/security.md).
