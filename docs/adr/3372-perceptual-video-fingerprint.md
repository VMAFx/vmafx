<!-- markdownlint-disable MD013 MD060 -->
# ADR-3372: Perceptual video fingerprint for near-duplicate result reuse

- **Status**: Proposed
- **Date**: 2026-10-24
- **Deciders**: lusoris
- **Tags**: `vmaf-tune`, `cache`, `ai`

## Context

`vmaf-tune` caches a trial result under the SHA-256 of the source file
(`tools/vmaf-tune/src/vmaftune/cache.py`, `cache_key()`). A re-encode, a remux
or a second download of the same title has other bytes, so it misses the cache
and every encode and score runs again.

[ADR-0618](0618-content-aware-classifier.md) plans a cheaper key, an xxHash128
of the first 1 MB of the file. That key fails in both directions:

- **Misses on the same content.** A re-encode or a remux changes the first
  megabyte, so the key changes.
- **Matches different content.** At 10 Mbit/s, 1 MB is under one second of
  video. Titles that open with the same ident, colour bars or black frames share
  their first megabyte while the rest differs. Reusing a score across them
  would return another title's quality.

A content key must therefore look at the pictures, not the bytes. It must hold
under a re-encode, a resolution change and a bitrate change. It must also
refuse a pair that only shares an opening.

## Decision

1. **Frame hashes.** `vmaf-tune` samples one frame per second with the host
   `ffmpeg` and hashes each frame with PDQ, through the `pdqhash` Python
   package (MIT, 0.2.8, built from source on Python 3.14). PDQ gives a 256-bit
   hash and a quality value from 0 to 100 per frame. It is the frame hash of
   the ThreatExchange vPDQ project, which matches videos the same way. The
   dependency is an optional extra (`vmaf-tune[fingerprint]`); without it,
   reuse stays exact-hash only and says so.
2. **Frame match.** Two frames match when their Hamming distance is at most 31
   of 256 bits, the match threshold the PDQ authors publish. Frames with a
   quality below 50 are dropped before matching, so flat black frames, fades
   and solid cards never count.
3. **Clip match and refusal.** A cached result is reused for a new source only
   when every step passes. Any failed step is a refusal: the trial runs as a
   miss and the refusal reason is logged.
   1. Duration within 0.5 s or 0.5 % (the larger), and the same display and
      pixel aspect ratio.
   2. At least 30 frames with a quality of 50 or more in each source.
      Otherwise the fingerprint is too weak to compare.
   3. Three segments, starting at 10 %, 50 % and 85 % of the duration. In each
      one, at least 85 % of the valid query frames have a match within one
      second of the same timestamp in the cached source. A shared opening
      cannot pass the 50 % and 85 % segments.
   4. Optional spot check (`--near-dup-verify`): one second of each source is
      scored with VMAF against the cached reference window. A difference above
      1.5 VMAF points refuses the pair and counts it as a conflict.
4. **Where it applies.** The exact `src_sha256` key stays primary. The
   fingerprint is a second lookup that runs only on an exact miss. A reused
   result keeps its cache key and records the fingerprint match, the matched
   source hash and the three segment ratios, so a report can show that a
   score came from a near-duplicate.
5. **The thresholds are measured, not assumed.** The values above are the
   starting point. Before the feature is on by default, a run over a real
   library reports the near-duplicate hit rate, the false-match count and the
   segment ratios. A false match moves the thresholds. Two planted pairs are
   held in the tests:
   - Same first megabyte, different content after it: refused.
   - A re-encode of the same clip at another resolution and bitrate: matched.
6. **No media leaves the host.** Frames and hashes are computed locally. No
   frame, clip or path is sent to any hosted service.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| PDQ frame hashes, vPDQ-style matching (chosen) | Permissive licence; published threshold; per-frame quality drops flat frames; CPU only | Needs frame sampling through `ffmpeg`; one compiled wheel | — |
| xxHash128 of the first 1 MB (ADR-0618) | No decode; instant | Misses re-encodes; matches titles that share an opening | Fails both planted pairs |
| FFmpeg `signature` filter (MPEG-7 video signature) | Standardised (ISO/IEC 15938-3 Amd. 4); no new dependency when `ffmpeg` is built with `--enable-gpl` | `libavfilter/vf_signature.c` is GPL-2.0-or-later and needs a GPL `ffmpeg`; coarse result | Kept as an out-of-process fallback only; never linked |
| Collage hash of a video (`videohash`) | One hash per video | One collage loses temporal order, so a shared opening and a re-cut look alike | Cannot meet the segment rule |
| pHash C library | Video DCT hash | GPL-3.0-or-later; dormant | Licence |
| `ImageHash` dHash or pHash per frame | BSD-2-Clause; pure Python | No frame-quality signal; flat frames collide | PDQ covers the same with a quality filter |
| TMK+PDQF (ThreatExchange) | Temporal model | Heavier; C++ build; aimed at large-scale matching | More than a per-title cache needs |
| Learned copy-detection embeddings | Robust to heavy edits | GPU and a model to ship and maintain | Out of scale for cache reuse |

## Consequences

- **Positive**: re-encodes and remuxes of a title reuse its results. The
  first-megabyte key of ADR-0618 is replaced before it ships. Every reuse is
  recorded and can be audited.
- **Negative**: fingerprinting decodes one frame per second of each new
  source. `vmaf-tune` gains an optional compiled dependency.
- **Neutral / follow-ups**: the ADR-0618 classifier uses this fingerprint as
  its cache key. The thresholds are revisited after the measured library run.
  The controller's submit path (`cmd/vmafx-controller`) may use the same key
  later; that change needs its own ADR.

## Supply-chain impact

- `pdqhash` 0.2.8 (MIT), optional extra only, pinned in the test lock. It
  builds from its source distribution, so the lock carries its hashes.
- `vpdq` 0.2.5 (BSD-style licence) was read for its matching rule and is not a
  dependency.

## References

- PDQ and vPDQ: ThreatExchange hashing documentation,
  <https://github.com/facebook/ThreatExchange/tree/main/pdq> and
  <https://github.com/facebook/ThreatExchange/tree/main/vpdq>.
- `pdqhash` Python binding: <https://github.com/faustomorales/pdqhash-python>.
- MPEG-7 video signature: S. Paschalakis et al., "The MPEG-7 Video Signature
  Tools for Content Identification", IEEE TCSVT 22(7), 2012,
  <https://doi.org/10.1109/TCSVT.2012.2189791>; FFmpeg `signature` filter,
  <https://ffmpeg.org/ffmpeg-filters.html#signature>.
- Operator order vmafx-near-dup-genre-20261024 (paraphrased): a perceptual
  fingerprint for near-duplicate result reuse with a stated threshold, a
  refusal path and planted negatives.
