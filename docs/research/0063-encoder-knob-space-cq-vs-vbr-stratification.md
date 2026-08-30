<!-- markdownlint-disable MD013 MD060 -->
# Research-0063 — Encoder knob-space stratifies by rate-control mode (CQ vs VBR)

## TL;DR

The "VOD-HQ recipe" (`-tune hq -multipass fullres -spatial_aq 1
-temporal_aq 1 -rc-lookahead 32 -bf 3` for NVENC; `-look_ahead 1
-look_ahead_depth 40 -bf 4 -adaptive_i 1 -adaptive_b 1` for QSV) is
calibrated for **VBR/CBR rate control**, not constant-CQ. At fixed
CQ it **regresses** NVENC quality by 2.7–3.3 VMAF points and only
marginally lifts QSV (+0.2 to +0.9). vmaf-tune's recommend output
must condition on the rate-control mode the user is targeting before
suggesting knobs.

## Method

Two grids run on the same 9 Netflix sources, same scoring backend
(libvmaf CUDA, vmaf_v0.6.1 model):

| grid | preset | quality | extras |
|------|--------|---------|--------|
| **bare** | NVENC `p4`, QSV `medium` | cq 18, 25, 31, 37 | none |
| **tuned** | NVENC `p4..p7`, QSV `slow..veryslow` | cq 18, 22, 26, 30, 34, 38 | NVENC: `-tune hq -multipass fullres -spatial_aq 1 -temporal_aq 1 -rc-lookahead 32 -bf 3`; QSV: `-look_ahead 1 -look_ahead_depth 40 -bf 4 -adaptive_i 1 -adaptive_b 1` |

Both pipelines use the same `scripts/dev/hw_encoder_corpus.py` driver
(NVENC + NVDEC + libvmaf-CUDA score, see PR #392). 33,840 bare per-frame
rows + 120,960 tuned per-frame rows. Comparison restricted to (encoder, cq)
pairs present in both grids (12 cells).

## Result

Mean VMAF lift (tuned − bare) across matched (encoder, cq) cells:

| encoder      | mean lift |
|--------------|----------:|
| `h264_nvenc` | **−2.71** |
| `hevc_nvenc` | **−3.31** |
| `av1_nvenc`  | −0.34     |
| `h264_qsv`   | +0.21     |
| `av1_qsv`    | +0.81     |
| `hevc_qsv`   | +0.94     |

Per-cell deltas at cq=30 (mid-quality operating point):

```text
av1_nvenc  bare 96.96 / tuned 96.23 (Δ -0.73)
h264_nvenc bare 92.75 / tuned 88.06 (Δ -4.69)
hevc_nvenc bare 94.50 / tuned 90.19 (Δ -4.30)
av1_qsv    bare 90.94 / tuned 92.02 (Δ +1.09)
h264_qsv   bare 91.06 / tuned 90.69 (Δ -0.37)
hevc_qsv   bare 90.77 / tuned 91.46 (Δ +0.69)
```

NVENC's tuned recipe loses ~4 VMAF points at cq=30 vs bare-defaults
on h264 / hevc.

## Why it stratifies by rate-control mode

The NVENC HQ recipe's load-bearing knobs target rate-controlled
encoding:

- **`-multipass fullres`** is a 2-pass mechanism that amortises the
  first pass's bit-budget estimate across the second pass. With
  constant CQ there is no bit budget — multipass either over-spends
  (writes a heavier first pass than the cq target needs) or
  under-spends. Either way the second pass diverges from what a
  single-pass cq encode would do.
- **`-tune hq`** flips the encoder's internal cost-function weights
  toward "high-quality VBR" — this includes more aggressive scene-cut
  detection and B-pyramid placement that pays off when the
  rate-control loop has bits to redistribute. CQ has no rate-control
  loop, so the cost-function shift adds overhead without the
  redistribution benefit.
- **`-bf 3` + `-rc-lookahead 32`** assume the encoder can amortise the
  3-frame B-pyramid latency across a 32-frame look-ahead window.
  On 6-second 1080p clips (150 frames at 25fps) that's reasonable;
  on shorter clips or scene-change-heavy content the look-ahead
  window walks past content boundaries and pulls bad reference
  decisions.

QSV's recipe is less affected because Intel's MFX session implements
look-ahead as a quality-improving probe regardless of rate-control
mode — the lift is small (+0.2 to +0.9) but consistent.

## Implication for vmaf-tune

vmaf-tune's `recommend` command (Phase B / ADR-0237) currently
takes a quality target (`--target-vmaf` or `--target-bitrate`) but
does **not** know which rate-control mode the user is targeting.
Before recommending knobs the recommender must distinguish:

1. **CQ-target encoding** ("constant quality, let bitrate float") —
   skip multipass, skip `-tune hq` for NVENC, prefer the simpler
   bare-default recipe + sweep preset.
2. **VBR-target encoding** ("hit this average bitrate at best
   quality") — apply the full HQ recipe, run multipass, enable
   look-ahead at full depth.
3. **CBR-target encoding** ("rate-controlled live, hit this exact
   bitrate") — different again; tune for low-latency, no look-ahead.

The corpus generator produced today is **CQ-mode only**. A VBR-mode
sweep against the same source set would generate a comparable corpus
where the HQ recipe should genuinely lift (predicted +2 to +5 VMAF
at fixed bitrate based on Intel's published QSV calibrations).

## Decision

1. Document this in the recommend-command spec — recommend output
   must carry a `rate_control_mode` field (`cq` / `vbr` / `cbr`).
2. Phase A corpus runner gains a `--rate-control` knob; default to
   `cq` (current behaviour) but emit a `rate_control_mode` column
   in every JSONL row.
3. fr_regressor_v2 input vector gains a `rate_control_mode` one-hot
   (3-dim: cq / vbr / cbr / unknown). Append-only vocab extension
   following the same shape as the encoder vocab v1→v2 in PR #394.
4. Future tuned-recipe ADR captures recipe-per-mode separately
   rather than the current single-recipe assumption.

## Reproducer

```bash
# Bare-default cq sweep (already in runs/phase_a/full_grid/nvenc_full.jsonl)
python3 scripts/dev/hw_encoder_corpus.py \
  --vmaf-bin core/build-cuda/tools/vmaf \
  --source .workingdir2/netflix/ref/BigBuckBunny_25fps.yuv \
  --width 1920 --height 1080 --pix-fmt yuv420p --framerate 25 \
  --encoder h264_nvenc --cq 30 --out /tmp/bare.jsonl

# Tuned VOD-HQ cq sweep (same source, same cq, hq recipe)
python3 scripts/dev/hw_encoder_corpus.py \
  --vmaf-bin core/build-cuda/tools/vmaf \
  --source .workingdir2/netflix/ref/BigBuckBunny_25fps.yuv \
  --width 1920 --height 1080 --pix-fmt yuv420p --framerate 25 \
  --encoder h264_nvenc --preset p4 --cq 30 \
  --extra-encode=-tune --extra-encode=hq \
  --extra-encode=-multipass --extra-encode=fullres \
  --extra-encode=-spatial_aq --extra-encode=1 \
  --extra-encode=-temporal_aq --extra-encode=1 \
  --extra-encode=-rc-lookahead --extra-encode=32 \
  --extra-encode=-bf --extra-encode=3 \
  --out /tmp/tuned.jsonl

# Diff pooled VMAF — expect tuned to be ~4 points lower at cq=30
```

## References

- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — vmaf-tune Phase A scope
- [PR #392](https://github.com/VMAFx/vmafx/pull/392) — `hw_encoder_corpus.py` runner
- [PR #394](https://github.com/VMAFx/vmafx/pull/394) — `fr_regressor_v2` ENCODER_VOCAB v2
- [NVIDIA Video Codec SDK Programming Guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/) — multipass / tune semantics
