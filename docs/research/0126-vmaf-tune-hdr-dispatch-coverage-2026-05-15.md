# Research-0126: `vmaf-tune` HDR dispatch coverage

- **Status**: Active
- **Workstream**: ADR-0300
- **Last updated**: 2026-05-15

## Question

`vmaftune.hdr.hdr_codec_args()` was the central encode-side HDR
contract, but it only dispatched `libx264`, `libx265`, `libsvtav1`,
`hevc_nvenc`, and `libvvenc`. The codec registry now contains more
HDR-capable encoder rows: AV1 NVENC, HEVC/AV1 QSV, HEVC/AV1 AMF, HEVC
VideoToolbox, and software AV1 via libaom. The open question was
whether to leave those rows unflagged until HDR model training lands,
or widen encode-side signaling now.

## Sources

- [ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md) defines the single
  dispatch-table contract and explicitly separates encode-side HDR
  correctness from the missing HDR VMAF model.
- [Research-0072](0072-vmaf-tune-hdr-aware.md) records the original
  HDR flag families: global `-color_*`, x265 private params, SVT-AV1
  params, and HEVC NVENC 10-bit output.
- [Research-0087](0087-vmaftune-codec-adapter-dispatcher-hp1.md)
  inventories the adapter registry shape that `hdr_codec_args()`
  needs to cover.

## Findings

Leaving the newer adapters out of `hdr_codec_args()` made the auto
planner and corpus path record `hdr_*` provenance while emitting an
empty `hdr_args` tuple for valid HDR-capable hardware codecs. That is
an encode-side correctness gap independent of CHUG or Netflix's future
HDR model release.

The least risky widening is:

- use codec-private HDR SEI flags only where the fork already has a
  stable mapping (`libx265`, `libsvtav1`, `hevc_nvenc`);
- force 10-bit output for hardware HEVC (`p010le` + `main10`);
- force 10-bit 4:2:0 output for hardware AV1 (`p010le`);
- carry FFmpeg-global `-color_*` tags for software AV1 and VVenC,
  where the in-tree evidence does not pin a stable private SEI option
  family.

## Alternatives explored

| Option | Verdict | Reason |
| --- | --- | --- |
| Keep the narrow table until CHUG training completes | Rejected | CHUG blocks model training, not encode-side color signaling. Empty HDR args for hardware HDR rows are avoidable now. |
| Move HDR argv into each codec adapter | Rejected | ADR-0300 deliberately made `hdr_codec_args()` the single auditable contract; scattering the logic would reintroduce per-adapter drift. |
| Guess private SEI flags for every hardware backend | Rejected | Some FFmpeg wrappers expose different or unstable private option names. Global color tags plus 10-bit output are the safe floor. |

## Open questions

- Add backend-specific mastering-display / MaxCLL private flags for
  QSV, AMF, VideoToolbox, and AV1 NVENC only after a real FFmpeg build
  matrix verifies those option names.
- HDR VMAF scoring remains blocked until either Netflix releases a
  licensed model artifact or the CHUG pipeline produces a fork-owned
  HDR MOS head suitable for a separate model PR.

## Related

- [ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md)
- [Research-0072](0072-vmaf-tune-hdr-aware.md)
- [Research-0101](0101-training-discovery-synthesis-2026-05-14.md)
