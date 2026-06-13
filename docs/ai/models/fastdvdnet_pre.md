# FastDVDnet temporal pre-filter (5-frame window)

`fastdvdnet_pre` — a *temporal* denoising pre-filter for noisy / grainy
sources. Unlike the per-frame `learned_filter_v1` (capability C3), this
extractor consumes a 5-frame sliding window
``[t-2, t-1, t, t+1, t+2]`` and emits a denoised version of frame
``t``. Denoise-before-encode is a well-validated bitrate lever: removing
sensor noise and grain before the encoder sees them produces smaller
files at the same subjective quality.

> **Status — real upstream weights (T6-7b).** Since T6-7b the shipped
> ONNX under `model/tiny/fastdvdnet_pre.onnx` carries real
> [m-tassano/fastdvdnet](https://github.com/m-tassano/fastdvdnet)
> weights (Tassano, Delon, Veit 2020; MIT license) wrapped by a thin
> luma adapter that preserves the C-side I/O contract. The previous
> smoke-only placeholder is documented in
> [ADR-0215](../../adr/0215-fastdvdnet-pre-filter.md); the weights swap
> rationale and the luma adapter design are recorded in
> [ADR-0255](../../adr/0255-fastdvdnet-pre-real-weights.md). The
> downstream FFmpeg `vmaf_pre_temporal` filter
> that consumes the denoised frame buffer is still tracked separately.

## What the output means

The extractor appends a single per-frame feature named
`fastdvdnet_pre_l1_residual`, defined as the mean-absolute difference
between the input centre frame `t` (normalised to `[0, 1]`) and the
denoised output. It exists so libvmaf's per-frame plumbing has a scalar
to record; it is **not** a quality metric. Downstream pipelines that
want the actual denoised pixel data should consume the FFmpeg
`vmaf_pre_temporal` filter once it lands — that filter writes the
denoised frame buffer to its output stream.

| Value | Interpretation |
| --- | --- |
| **~0.0** | Output ≈ input centre (quiet / flat content; little to denoise). |
| **~0.05** | Typical residual on lightly noisy content. |
| **~0.20+** | Heavy denoising on grainy / film content. |

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model name | `vmaf_tiny_fastdvdnet_pre_v1` |
| Location | `model/tiny/fastdvdnet_pre.onnx` |
| Size | ~9.5 MiB (2.48M parameters from upstream FastDVDnet) |
| ONNX opset | 17 |
| Input | `frames` — float32 NCHW `[1, 5, H, W]` (5-frame luma stack along channel axis) |
| Output | `denoised` — float32 NCHW `[1, 1, H, W]` (denoised frame `t`) |
| License | MIT (upstream m-tassano/fastdvdnet, see provenance below) |
| Smoke flag | `smoke: false` in registry — real working denoiser |

The sidecar JSON at `model/tiny/fastdvdnet_pre.json` carries the input /
output names plus `frame_window: 5`, `centre_index: 2` so downstream
consumers can validate the contract without parsing the ONNX graph. It
also records the upstream commit pin and weight checksum for
reproducibility. Fresh exports add ADR-0661 `run_provenance` with the
requested upstream directory, `model.pth`, `models.py`, parsed exporter
arguments, ONNX output, sidecar output, and registry target.

## Provenance and license attribution

The shipped weights are the verbatim trained parameters from upstream
FastDVDnet, repackaged for the fork's luma I/O contract. License terms
are unchanged from upstream MIT.

| Field | Value |
| --- | --- |
| Upstream repo | [github.com/m-tassano/fastdvdnet](https://github.com/m-tassano/fastdvdnet) |
| Upstream commit | `c8fdf6182a0340e89dd18f5df25b47337cbede6f` (2024-02-01) |
| Upstream weights file | `model.pth` |
| Upstream weights sha256 | `9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17` |
| Upstream license | MIT, Copyright 2024 Matias Tassano `<mtassano@meta.com>` |
| Citation | Tassano, Delon, Veit. *FastDVDnet: Towards Real-Time Deep Video Denoising Without Flow Estimation*, CVPR 2020. arXiv:1907.01361. |

## Luma adapter design

Upstream FastDVDnet was trained on RGB inputs with an explicit per-pixel
noise map; the fork's C extractor was scoped luma-only with a fixed
`(1, 5, H, W)` input contract (ADR-0215). To ship real weights without
breaking the C surface, the export script
(`ai/scripts/export_fastdvdnet_pre.py`) wraps upstream's RGB graph in a
small `LumaAdapter` module:

1. **Y → [Y, Y, Y] tile.** Each of the five luma planes is replicated
   into the three RGB channels via `Concat` (allowlist-safe), producing
   the upstream-expected `(1, 15, H, W)` tensor.
2. **Constant noise map.** A constant `sigma = 25/255 ≈ 0.098` (the
   reference inference noise level used by upstream's
   `test_fastdvdnet.py`) is broadcast to `(1, 1, H, W)` via
   `ones_like(centre) * sigma` and fed as the second upstream input.
3. **RGB → Y collapse.** The upstream RGB output is converted back to
   luma using BT.601 weights `Y = 0.299 R + 0.587 G + 0.114 B`.

Trade-offs:

- **Pros**: real working denoiser; zero changes to the C extractor or
  its tests; upstream weights drop is reproducible from the pinned
  `model.pth` checksum.
- **Cons**: the network was not trained on luma-tiled-into-RGB inputs,
  so the denoising quality on luma alone is below what a luma-native
  retrain would achieve. The adapter remains numerically identical to
  the upstream graph for any RGB content; it is the input-domain
  mismatch that costs quality.
- **Follow-up**: a luma-native retrain (or chroma-aware variant) is
  tracked under T6-7c — depends on the FFmpeg `vmaf_pre_temporal`
  filter shipping first so the retrain target reflects the real
  consumer.

## Op allowlist compliance

`PixelShuffle` in upstream's UpBlocks would export to `DepthToSpace`,
which is NOT on the fork's strict ONNX op allowlist
([`core/src/dnn/op_allowlist.c`](../../../core/src/dnn/op_allowlist.c)).
The export script swaps every `nn.PixelShuffle` instance for an
allowlist-safe `Reshape`/`Transpose`/`Reshape` decomposition before
exporting. PixelShuffle has zero learned parameters, so the swap is
numerically identical (verified in CI: `max abs diff < 1e-6` between
upstream PyTorch and exported ONNX on random inputs).

The full op set in the shipped graph is `Add`, `Cast`, `Clip`,
`Concat`, `Constant`, `ConstantOfShape`, `Conv`, `Div`, `Gather`,
`Identity`, `Mul`, `ReduceSum`, `Relu`, `Reshape`, `Shape`, `Slice`,
`Sub`, `Transpose`, `Unsqueeze` — every one already on the allowlist.

## Frame window contract

The C extractor (`core/src/feature/fastdvdnet_pre.c`) maintains a
five-slot ring buffer of the most recent normalised luma planes. Each
`extract()` call:

1. Pushes the new luma plane (any bpc; rescaled to `[0, 1]`) into the
   ring at `next_slot`.
2. Gathers the five window slots into a `[1, 5, H, W]` input tensor.
   At clip start (when fewer than 5 frames have been seen) and clip end
   (no future frames available) the missing slots replicate the closest
   available end frame — a replicate-edge convention that matches
   FastDVDnet's published reflection-pad-light behaviour.
3. Calls `vmaf_dnn_session_run` with the named bindings
   `frames` (input) and `denoised` (output).
4. Computes the L1 residual against the centre input frame and appends
   it via `vmaf_feature_collector_append`.

## Integration recipe

```bash
# 1. Build libvmaf with DNN support enabled.
meson setup core/build-cpu -Denable_dnn=enabled
ninja -C core/build-cpu

# 2. Run the extractor against a clip, supplying the model path.
core/build-cpu/tools/vmaf \
    --reference ref.yuv --distorted dis.yuv \
    --width 1920 --height 1080 --pixel_format yuv420p --bitdepth 8 \
    --feature fastdvdnet_pre=model_path=model/tiny/fastdvdnet_pre.onnx

# Or via env var (matches lpips_sq's pattern):
VMAF_FASTDVDNET_PRE_MODEL_PATH=model/tiny/fastdvdnet_pre.onnx \
    core/build-cpu/tools/vmaf --feature fastdvdnet_pre …
```

The extractor declines cleanly (non-fatal `-EINVAL`) if neither
`model_path` nor `VMAF_FASTDVDNET_PRE_MODEL_PATH` is set, the same
contract as the LPIPS extractor.

## Reproducing the export

```bash
# 1. Fetch upstream pinned at the recorded commit.
mkdir -p /tmp/fastdvdnet_upstream
cd /tmp/fastdvdnet_upstream
curl -L -O https://raw.githubusercontent.com/m-tassano/fastdvdnet/c8fdf61/model.pth
curl -L -O https://raw.githubusercontent.com/m-tassano/fastdvdnet/c8fdf61/models.py

# 2. Verify the upstream weights checksum (export script also enforces this).
sha256sum model.pth
# expect: 9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17

# 3. Run the exporter from the fork root.
cd /path/to/vmaf
python3 ai/scripts/export_fastdvdnet_pre.py \
    --upstream-dir /tmp/fastdvdnet_upstream \
    --output model/tiny/fastdvdnet_pre.onnx
```

The script auto-updates `model/tiny/registry.json` and writes the
sidecar `model/tiny/fastdvdnet_pre.json`. Rerunning is idempotent
provided the upstream checksum matches.

## Smoke test

The C-side registration + options-table contract is exercised by
`core/test/test_fastdvdnet_pre.c`, which runs in every build (no ORT
session required). To exercise the live ORT path against the shipped
ONNX, enable DNN and run the suite:

```bash
meson test -C build --suite=fast --print-errorlogs test_fastdvdnet_pre
```

## References

- Tassano, Delon, Veit. *FastDVDnet: Towards Real-Time Deep Video
  Denoising Without Flow Estimation*, CVPR 2020. arXiv:1907.01361.
- Reference implementation:
  [github.com/m-tassano/fastdvdnet](https://github.com/m-tassano/fastdvdnet)
- [ADR-0215](../../adr/0215-fastdvdnet-pre-filter.md) — placeholder
  rationale and contract design.
- [ADR-0255](../../adr/0255-fastdvdnet-pre-real-weights.md) — T6-7b
  real-weights drop and luma adapter design.
- [Roadmap §3.3](../roadmap.md) — Wave 1 schedule.
