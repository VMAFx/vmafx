<!-- markdownlint-disable MD060 -->
# vmaf-tune

Quality-aware encode automation harness for the lusoris vmaf fork. Drives
`ffmpeg` over an encoder-parameter grid, scores each encode with the
`vmaf` CLI, and ships a JSONL corpus plus a stack of higher-level
subcommands that build on it (target-VMAF bisect, per-title CRF, per-shot
zones, saliency-aware ROI, bitrate ladder, HDR-aware tuning, fast-path
prediction, codec comparison).

User documentation: [`docs/usage/vmaf-tune.md`](../../docs/usage/vmaf-tune.md).

The Phase A scaffold landed via
[ADR-0237](../../docs/adr/0237-quality-aware-encode-automation.md); subsequent
expansions are tracked under separate ADRs (codec adapters
[0288](../../docs/adr/0288-vmaf-tune-codec-adapter-x265.md) /
[0290](../../docs/adr/0290-vmaf-tune-nvenc-adapters.md) and siblings,
per-shot tuning [0392](../../docs/adr/0392-vmaf-tune-phase-d-per-shot.md),
fast-path [0276](../../docs/adr/0276-vmaf-tune-fast-path.md), bitrate
ladder [0295](../../docs/adr/0295-vmaf-tune-phase-e-bitrate-ladder.md),
GPU score backend
[0314](../../docs/adr/0314-vmaf-tune-score-backend-vulkan.md) — the
Vulkan option was later removed by
[ADR-0726](../../docs/adr/0726-drop-vulkan-backend.md), …).

## Codec adapters

17 adapters under `src/vmaftune/codec_adapters/` — pick the one that
matches the encoder you have available locally:

| Family | Software         | NVIDIA NVENC      | Intel QSV         | AMD AMF           | Apple VideoToolbox      |
|--------|------------------|-------------------|-------------------|-------------------|-------------------------|
| AV1    | `libaom`, `svtav1` | `av1_nvenc`     | `av1_qsv`         | `av1_amf`         | —                       |
| H.264  | `x264`           | `h264_nvenc`      | `h264_qsv`        | `h264_amf`        | `h264_videotoolbox`     |
| HEVC   | `x265`           | `hevc_nvenc`      | `hevc_qsv`        | `hevc_amf`        | `hevc_videotoolbox`     |
| VVC    | `vvenc`          | —                 | —                 | —                 | —                       |

Per-adapter caveats (preset mapping, CRF range, host requirements) are
captured in `docs/usage/vmaf-tune.md` §"Codec adapters".

## Subcommands

- `corpus` — grid-sweep encoder parameters, score each output, emit
  JSONL.
- `recommend` — target-VMAF bisect (Phase B); pick a CRF that hits a
  requested score on a held-out clip.
- `tune-per-shot` — per-shot CRF zones (consumes the
  [`vmaf-perShot`](../vmaf-perShot/) plan).
- `recommend-saliency` — saliency-aware ROI tuning (consumes
  [`vmaf-roi`](../vmaf-roi/) sidecars).
- `ladder` — per-title bitrate ladder construction.
- `fast` — predicted-CRF fast path (skip the bisect when the regressor
  is confident).
- `hdr` — HDR-aware encoding + HDR-VMAF scoring.
- `compare` — apples-to-apples codec comparison at matched VMAF.

## Layout

```text
tools/vmaf-tune/
  pyproject.toml
  vmaf-tune                       # console entry-point shim
  src/vmaftune/
    __init__.py                   # version + public API
    cli.py                        # argparse wiring
    encode.py                     # ffmpeg driver (subprocess)
    score.py                      # vmaf binary driver (subprocess)
    corpus.py                     # grid sweep orchestrator + JSONL writer
    compare.py                    # codec-comparison subcommand
    fast.py                       # predicted-CRF fast path
    hdr.py                        # HDR-aware tuning
    ladder.py                     # per-title bitrate ladder
    recommend.py                  # target-VMAF bisect (Phase B)
    saliency.py                   # saliency-weighted ROI tuning
    codec_adapters/
      __init__.py
      x264.py, x265.py, libaom.py, svtav1.py, vvenc.py     # software
      av1_nvenc.py,  h264_nvenc.py,  hevc_nvenc.py         # NVIDIA NVENC
      av1_qsv.py,    h264_qsv.py,    hevc_qsv.py           # Intel QSV
      av1_amf.py,    h264_amf.py,    hevc_amf.py           # AMD AMF
      h264_videotoolbox.py,         hevc_videotoolbox.py   # Apple VideoToolbox
      _nvenc_common.py, _qsv_common.py, _amf_common.py,
      _videotoolbox_common.py, _gop_common.py              # shared helpers
  tests/
    test_corpus.py                # smoke test (mocks subprocess)
    ...
```

## Quick start

```bash
# from repo root
pip install -e tools/vmaf-tune
vmaf-tune corpus \
    --source path/to/ref.yuv --width 1920 --height 1080 --pix-fmt yuv420p \
    --preset medium --preset slow \
    --crf 22 --crf 28 --crf 34 \
    --output corpus.jsonl
```

Each emitted row has the schema documented in
[`docs/usage/vmaf-tune.md`](../../docs/usage/vmaf-tune.md). The schema is
the API contract every downstream subcommand consumes; do not change it
without bumping `SCHEMA_VERSION` in `src/vmaftune/__init__.py`.

## Predictor Training

The offline trainer accepts either one corpus JSONL file or a directory
of sharded JSONL files:

```bash
python -m vmaftune.predictor_train \
  --corpus .workingdir2/corpus_run \
  --codec libx264 \
  --output-dir .workingdir2/predictor-real
```

Directory inputs are scanned recursively in sorted order. Per-codec rows
are filtered after normalising both current `corpus.py` keys and older
hardware-sweep aliases, so existing `.workingdir2` corpora can be used
without concatenating them first.

## Tests

```bash
pytest tools/vmaf-tune/tests/
```

The shipped smoke mocks `subprocess.run` so it requires neither `ffmpeg`
nor a built `vmaf` binary.
