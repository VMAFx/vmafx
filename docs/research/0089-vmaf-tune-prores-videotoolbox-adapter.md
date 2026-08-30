# Research-0089: `vmaf-tune` Apple ProRes VideoToolbox codec adapter

- **Date**: 2026-05-09
- **Status**: Implementation digest (companion to the *Status update
  2026-05-09* appendix in ADR-0283).
- **Tags**: tooling, ffmpeg, codec, hardware-encoder, apple, prores,
  fork-local
- **Companion ADR**: [ADR-0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md)
  (Status update 2026-05-09)

## Question

Complete the macOS hardware-encoder coverage trio in `tools/vmaf-tune/`
by adding the `prores_videotoolbox` adapter alongside the existing
`h264_videotoolbox` + `hevc_videotoolbox` adapters from ADR-0283.
ProRes is the prosumer / broadcast intermediate codec on Apple
platforms; the adapter unblocks ProRes encodes from the same
codec-adapter registry the search loop already consumes.

The shape question: ProRes is **not** quality-controlled like H.264 /
HEVC. There is no CRF, no QP, no `-q:v`. How does it fit the
harness's generic `crf` slot without breaking the
"search-loop-never-branches-on-codec" invariant?

## Findings

1. **Encoder name and FFmpeg surface.** FFmpeg exposes
   `prores_videotoolbox` as a third VideoToolbox-backed encoder. The
   AVOption table is declared in `libavcodec/videotoolboxenc.c`
   `prores_options` (verified 2026-05-09 against an FFmpeg n8.1.1
   checkout). The encoder shares `COMMON_OPTIONS` with the H.264 and
   HEVC VT encoders, so `-realtime`, `-allow_sw`, `-require_sw` work
   identically across the three.

2. **Quality knob — fixed-rate, tier-selected.** ProRes is a
   fixed-rate intermediate codec. The `profile` AVOption selects the
   tier and, by extension, the implicit bitrate. From
   `prores_options` (FFmpeg n8.1.1):

   | int (`AV_PROFILE_PRORES_*`) | FFmpeg alias | Marketing name |
   | --- | --- | --- |
   | 0 | `proxy` | ProRes 422 Proxy |
   | 1 | `lt` | ProRes 422 LT |
   | 2 | `standard` | ProRes 422 |
   | 3 | `hq` | ProRes 422 HQ |
   | 4 | `4444` | ProRes 4444 |
   | 5 | `xq` | ProRes 4444 XQ |

   The encoder accepts the integer or the named alias on the
   command line; the adapter emits the named alias for
   diagnosability.

3. **Adapter shape.** The harness's `crf` row slot is a generic
   "quality knob" the search loop dials uniformly across codecs
   (per ADR-0237). Because ProRes' tier id is also a monotonically
   increasing integer where higher = "better" (more bits, more
   chroma precision), it slots cleanly into the same field with
   `quality_range=(0, 5)` and `invert_quality=False`. No row-schema
   change in `corpus.py`; downstream consumers translate the
   integer back to a tier name via the adapter registry. This
   preserves the search-loop invariant.

4. **Preset axis.** Same nine-name → `-realtime` boolean mapping as
   the H.264 / HEVC siblings. Reuses `_videotoolbox_common.py`'s
   existing `_PRESET_TO_REALTIME` table. ProRes' rate-distortion
   curve is dominated by tier choice, so the preset axis mostly
   affects throughput.

5. **Hardware availability.** Apple ships the dedicated ProRes
   hardware block on M1 Pro / M1 Max / M1 Ultra and every later
   M-series chip. The base M1 SoC (and all Intel Macs, including
   T2-equipped Macs) do **not** have it; FFmpeg's
   `prores_videotoolbox` falls back to the software `prores_aw` /
   `prores_ks` encoders on those hosts. The adapter does not gate
   on this — it lets FFmpeg report the unavailable-encoder error
   like the existing VT adapters do.

6. **`ENCODER_VOCAB` interaction.** The live proxy
   (`fr_regressor_v2`, ADR-0291) consumes `ENCODER_VOCAB_V2` (12
   slots, frozen). The scaffold-only `ENCODER_VOCAB_V3`
   (ADR-0302, 16 slots) does not include `prores_videotoolbox`
   either. Adding ProRes here is **adapter-only**, mirroring how
   ADR-0283 deferred the VT vocab expansion to a separate retrain
   PR. The proxy fast path raises `ProxyError` on ProRes input;
   the live-encode loop is unaffected. A future v4 retrain
   (T-FR-V2-VOCAB-V3-RETRAIN scope) will add the slot.

7. **Subprocess seam.** Tests mock `subprocess.run` exactly like
   the H.264 / HEVC VT tests; the suite stays Linux-CI-runnable.
   End-to-end ProRes exercise is left to contributors with an
   M-series Mac that has the ProRes hardware block.

## Decision matrix

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| Sibling adapter sharing `_videotoolbox_common.py` (chosen) | Matches the established VT pattern; reuses preset → realtime mapping; tier-id-as-`crf` preserves the row schema; no search-loop branching. | Adds a few ProRes-specific constants to the shared module. | **Selected** — minimal new surface, maximum convention reuse. |
| Drop ProRes into the same file as H.264 / HEVC VT adapters | One file holds the whole VT family. | Breaks the one-file-per-codec convention; mixes a fixed-rate codec with quality-controlled siblings. | Rejected — codec is materially different. |
| Add a separate `quality_kind` enum (`crf` / `qp` / `tier`) to the adapter contract | Future-proofs for radically different codecs. | Forces every adapter to declare it; threads through the search loop; ADR-0237 deliberately kept the contract narrow. | Deferred — not justified by a single new fixed-rate codec. |
| Wait for the v4 vocab retrain before landing the adapter | Keeps adapters and proxy in lockstep. | Blocks live-encode users; ADR-0283 already established the "adapter ships, vocab follows" pattern; ProRes corpus rows are useful even without proxy support. | Rejected — proxy gating is graceful (`ProxyError` plus live-encode fallback). |

## Reproducer

```bash
python -m pytest \
    tools/vmaf-tune/tests/test_codec_adapter_prores_videotoolbox.py -v
```

Expected: 22 passed.

## References

- [ADR-0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) — original
  VT adapters; *Status update 2026-05-09* appendix records the
  ProRes follow-on.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) —
  codec-adapter contract.
- [ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md) —
  `ENCODER_VOCAB_V2` 12-slot freeze.
- [ADR-0302](../adr/0302-encoder-vocab-v3-schema-expansion.md) —
  16-slot v3 scaffold; ProRes will land in a future v4.
- [Research-0074](0074-vmaf-tune-videotoolbox-adapters.md) — sibling
  H.264 / HEVC research digest.
- FFmpeg `libavcodec/videotoolboxenc.c` (`prores_options`,
  `ff_prores_videotoolbox_encoder`) — primary source for tier ids
  and AVOption shape.
