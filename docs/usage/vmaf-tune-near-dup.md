<!-- markdownlint-disable MD060 -->
# Perceptual video fingerprinting and near-duplicate cache reuse

The `vmaf-tune` cache stores encode and score results under a SHA-256 digest of
the reference video bytes. Re-encoded, remuxed, or transcoded versions of the
same title have different file hashes, which causes an exact-hash cache miss and
triggers unnecessary re-encoding and scoring runs.

Perceptual video fingerprinting provides an opt-in secondary cache lookup on an
exact hash miss. When enabled, `vmaf-tune` computes a perceptual fingerprint of
the video frames using PDQ hashing, matches it against cached fingerprints, and
reuses cached results when the visual content meets strict similarity criteria.

The full design and rationale are documented in
[ADR-3372](../adr/3372-perceptual-video-fingerprint.md).

## Installation

Fingerprinting requires the `pdqhash` (>= 0.2.8) and `numpy` packages. Install
them using the optional `fingerprint` extra:

```bash
pip install 'vmaf-tune[fingerprint]'
```

If fingerprinting is requested but the required packages are not installed,
`vmaf-tune` raises an `ImportError` explicitly naming `vmaf-tune[fingerprint]`.
It never silently falls back to unverified execution.

## How it works

1. **Frame sampling and hashing**: The source video is sampled at 1 frame per
   second via FFmpeg as raw RGB24 frames. Each sampled frame is hashed using
   PDQ into a 256-bit hash and a quality score from 0 to 100.
2. **Quality filtering**: Frames with a quality score below 50 are dropped to
   prevent flat solid cards, black screens, or fades from creating false
   matches.
3. **Exact miss, near-dup lookup**: The primary cache lookup by `src_sha256`
   always runs first. If an exact match is missing and `--near-dup-reuse` is
   enabled, the cache evaluates candidates sharing the same encode and score
   parameters (`params_key`).
4. **Refusal ladder**: Candidates are evaluated sequentially against a strict
   decision ladder. If any stage fails, the candidate is refused with a specific
   reason and execution proceeds with a standard encode.

## Matching thresholds and refusal ladder

Candidates must satisfy all criteria in the following order:

| Stage | Refusal reason | Condition | Threshold |
|---|---|---|---|
| 1 | `duration` | Duration difference | $\le \max(0.5\text{ s}, 0.5\% \text{ of duration})$ |
| 1 | `aspect` | Geometry | Display Aspect Ratio (DAR) and Pixel Aspect Ratio (PAR) must match |
| 2 | `too-few-frames` | Frame count | Both query and cached video must have $\ge 30$ valid frames (quality $\ge 50$) |
| 3 | `segment` | Temporal consistency | Segments centered at 10%, 50%, and 85% of duration must each have $\ge 85\%$ frame matches within $\pm 1.0\text{ s}$ |
| 4 | `spot-check-drift` | Optional spot check | VMAF score difference over a 1-second reference window must be $\le 1.5$ points |

Individual frames match when their Hamming distance is $\le 31$ out of 256 bits.

The 3-segment check ensures that videos that only share an identical opening,
intro, or logo sequence are strictly refused at the 50% and 85% segments.

## Command-line options

| Flag | Subcommands | Description |
|---|---|---|
| `--near-dup-reuse` | `corpus`, `recommend` | Enable perceptual fingerprint secondary cache lookup on exact cache miss. Default: off. |
| `--near-dup-verify` | `corpus`, `recommend` | Run a 1-second VMAF spot check against the reference; refuse the near-duplicate when the two scores differ by more than 1.5. Default: off. |

## Python API

To use near-duplicate reuse programmatically with `CorpusOptions`:

```python
from pathlib import Path

from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows

opts = CorpusOptions(
    encoder="libx264",
    output=Path("corpus.jsonl"),
    cache_enabled=True,
    cache_dir=Path.home() / ".cache" / "vmaf-tune",
    near_dup_reuse=True,
    near_dup_verify=True,
)
```

When a near-duplicate cache hit occurs, the returned result record includes:

- `near_dup_match`: `True`
- `near_dup_matched_src_sha256`: SHA-256 of the original reference source
- `near_dup_segment_ratios`: Match ratios for the 10%, 50%, and 85% segments
