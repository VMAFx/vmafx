# Research-0072 — `vmaf-tune` HDR-aware encoding + scoring

- **Status**: digest closed; ADR-0295 Accepted (encode-side); HDR-VMAF
  scoring deferred (no fork-local model JSON yet).
- **Date**: 2026-05-03
- **Author**: Lusoris / Claude
- **Companion ADR**: [ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md).
- **Parent**: [ADR-0237](../adr/0237-quality-aware-encode-automation.md)
  (vmaf-tune Phase A umbrella).

## TL;DR

HDR signaling is identifiable from container metadata alone. ffprobe
surfaces enough information (transfer characteristic, primaries,
optional mastering-display + content-light SEI side data) to classify
PQ / HLG / SDR with high confidence. The encode-side action is a pure
ffmpeg argv-construction refactor: per-codec dispatch tables exist
for x264 / x265 / SVT-AV1 / NVENC / VVenC and overlap heavily on
the global `-color_*` set. The score-side action — swapping the SDR
VMAF model for the HDR-trained one — is gated on porting Netflix's
`vmaf_hdr_v0.6.1.json` artifact, which is out of scope for this PR.
Decision: ship encode-side correctness now, file the model port as a
backlog item, fall back to SDR scoring with a logged warning until
the port lands.

## Detection accuracy

ffprobe's `color_transfer` / `color_primaries` / `color_space` fields
are populated reliably for HEVC, AV1, VP9, and H.264 streams that
carry the corresponding VUI / SEI signaling. The two ambiguous cases:

1. **Raw YUV reference clips** — no container, no metadata. Handled
   via the `--force-hdr-pq` / `--force-hdr-hlg` CLI overrides.
2. **Malformed containers** — PQ transfer with BT.709 primaries
   appears occasionally in test fixtures from broken transcoders.
   The implementation treats this as SDR (see ADR-0295 Decision §5).
   Failure mode is recoverable (HDR scores trend low, user re-runs
   with `--force-*`).

## Codec dispatch coverage

| Encoder | Carrier | Notes |
| --- | --- | --- |
| `libx264` | Container `-color_*` | x264 has no in-stream HDR SEI. |
| `libx265` | `-x265-params` | Includes `master-display=` + `max-cll=` when SEI present. |
| `libsvtav1` | `-svtav1-params` | AV1 enums: prim=9, transfer=16/18, matrix=9. |
| `hevc_nvenc` | `-pix_fmt p010le -profile:v main10` | Plus global `-color_*` + ffmpeg `-master_display` / `-max_cll`. |
| `libvvenc` | Container `-color_*` | SEI options behind `--vvenc-params` in newer ffmpeg builds. |

Phase A wires `libx264` only via the codec-adapter registry; the
other dispatch rows ship in this PR so the upcoming codec-adapter PRs
inherit a working HDR path the day they land.

## HDR VMAF model — why deferred

Netflix maintains an HDR-trained VMAF model
(`vmaf_hdr_v0.6.1.json`) in their internal research artifact. It is
not under the same BSD-3-Clause-Plus-Patent license as the SDR
models in `model/`; porting it requires a license review +
retraining-data provenance check that exceeds this PR's scope.

Options considered for the score-side gap:

1. **Block on the model port** — ship encode + score together. Indefinite
   delay. *Not chosen.*
2. **Ship encode-only, fall back to SDR scoring with a warning**
   (chosen). HDR encode-side correctness ships now; scoring follow-up
   is a one-file drop into `model/` plus an entry in the model
   registry.
3. **Train our own HDR VMAF model from scratch** — out of scope; the
   tiny-AI training pipeline exists (`ai/`) but HDR-corpus training
   data does not.

The implementation already resolves `model/vmaf_hdr_*.json` via
`select_hdr_vmaf_model`; the port is "drop a JSON file, add a row to
the model registry, done."

## Schema-v2 fields

| Key | Type | Use |
| --- | --- | --- |
| `hdr_transfer` | `""` / `"pq"` / `"hlg"` | Phase B / C consumers gate on this. |
| `hdr_primaries` | raw ffprobe string | Provenance — exact ffprobe value preserved. |
| `hdr_forced` | bool | True iff override flag bypassed detection. |

Phase B / C loaders treat missing keys as SDR for backward compat
with v1 rows.

## Test coverage shipped

`tools/vmaf-tune/tests/test_hdr.py` covers:

- Detection: SDR / PQ / HLG / mismatched-primaries / missing-file /
  ffprobe-failure / invalid-JSON.
- Codec dispatch: shape per encoder (x264, x265 PQ, x265 HLG,
  SVT-AV1 PQ, SVT-AV1 HLG, NVENC HEVC, unknown encoder).
- Model resolution: empty dir / shipped model / multi-version
  pick-latest / missing dir.
- Corpus integration: end-to-end `force-hdr-pq` (verify HDR fields
  in row + `-color_*` in encode argv) and `force-sdr` (verify HDR
  fields empty + no `-color_*`).

## Follow-up backlog

1. **Port `vmaf_hdr_v0.6.1.json`** (gated on Netflix-license review).
   Once landed, `select_hdr_vmaf_model` picks it up automatically;
   no `vmaftune` change required.
2. **Real-binary integration tests** — piggyback on the codec-adapter
   PRs (libx265 / libsvtav1) so an actual HDR YUV → encode → score
   round-trip lands with each adapter.
3. **HDR10+ dynamic metadata** — beyond Bucket #9 scope. Static
   HDR10 mastering-display SEI is what the encoders accept today;
   HDR10+ requires the codec-private dynamic-metadata interface
   (x265 `--dhdr10-info`, ffmpeg `-master_display_color_volume`).

## References

- ITU-T H.265 (08/2021) D.3.27 (mastering-display SEI).
- AV1 spec §6.4.2 (color_config OBU).
- ffmpeg color flag reference: <https://ffmpeg.org/ffmpeg-codecs.html#libx265>,
  <https://ffmpeg.org/ffmpeg-codecs.html#SVT_002dAV1>.
- Parent: [ADR-0237](../adr/0237-quality-aware-encode-automation.md)
  (vmaf-tune Phase A umbrella).
- Sibling Phase A audit bucket source: PR #354 — Bucket #9 row.
