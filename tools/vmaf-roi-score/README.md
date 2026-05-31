<!-- markdownlint-disable MD060 -->
# vmaf-roi-score (Option C)

Region-of-interest VMAF *scoring* for the lusoris vmaf fork.

> **Note on naming**: the existing `core/tools/vmaf_roi.c` (ADR-0247)
> ships a binary named `vmaf-roi` that emits per-CTU QP-offset sidecars
> for *encoder steering*. This package is the *scoring* counterpart —
> different surface, different output, related model. The names diverge
> deliberately so callers cannot confuse them.

Option C drives the `vmaf` CLI twice (once full-frame, once with a
saliency-masked distorted YUV) and emits a JSON record combining the
two pooled scores via a user-controlled weight:

```text
roi_vmaf = (1 - w) * vmaf_full + w * vmaf_masked
```

Option A (per-pixel feature pooling weighted by saliency in libvmaf C
code) is **not** in this PR — see
[ADR-0296](../../docs/adr/0296-vmaf-roi-saliency-weighted.md) for the
phased roadmap and the reasons Option C ships first.

User documentation: [`docs/usage/vmaf-roi-score.md`](../../docs/usage/vmaf-roi-score.md).

## Layout

```text
tools/vmaf-roi-score/
  pyproject.toml
  vmaf-roi-score                 # console entry-point shim (thin wrapper)
  src/vmafroiscore/
    __init__.py                  # version + public API (blend_scores)
    cli.py                       # argparse wiring
    score.py                     # vmaf binary driver (subprocess)
    mask.py                      # saliency mask materialiser
  tests/
    test_combine.py              # smoke test (mocks subprocess)
```

## Status

| Surface | Status |
|---|---|
| Combine math (`blend_scores`) | shipped, tested |
| CLI (`--reference / --distorted / --weight / --synthetic-mask`) | shipped, tested |
| `vmaf` subprocess seam | shipped, tested (mocked) |
| `--saliency-model` ONNX inference | shipped for little-endian planar 8/10/12/16-bit YUV |
| Per-pixel saliency-weighted pooling (Option A) | deferred — separate PR + ADR follow-up |

## Quick start

```bash
# from repo root
pip install -e tools/vmaf-roi-score
pip install -e 'tools/vmaf-roi-score[runtime]'

# Smoke (no ONNX) — synthetic mask exercises materialisation + combine plumbing
vmaf-roi-score \
    --reference /path/to/ref.yuv --distorted /path/to/dis.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --synthetic-mask 0.5 --threshold 0 --fade 1 --weight 0.7

# Real saliency mask materialisation
vmaf-roi-score \
    --reference /path/to/ref.yuv --distorted /path/to/dis.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --saliency-model model/tiny/saliency_student_v1.onnx \
    --threshold 0.3 --fade 0.1 --weight 0.7
```

## Testing

```bash
pytest tools/vmaf-roi-score/tests
```

The tests mock `subprocess.run` so no `vmaf` binary is needed.

## See also

- ADR-0296 — vmaf-roi-score saliency-weighted (Option C + Option A roadmap)
- Research-0063 — option-space digest (Option A vs B vs C)
- ADR-0247 — `core/tools/vmaf_roi.c` (the encoder-steering sibling)
- ADR-0286 — `saliency_student_v1` (the upstream-of-this saliency model)
- ADR-0042 — tiny-AI docs-required-per-PR rule
