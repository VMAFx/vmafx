<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-3382: The encode service leaves VMAFx for its own repository, torcul

- **Status**: Accepted
- **Date**: 2026-10-27
- **Deciders**: Lusoris
- **Tags**: api, platform, encode, governance

## Context

[ADR-3378](3378-encode-job-api.md) placed an encode-job API inside VMAFx: one
video stream in, a verified video-only stream plus provenance out, `auto` rate
control only, in eight phased slices (issues #2986 to #2993). No slice had
landed code.

On 2026-10-27 the operator decided that the encode service becomes its own
repository in the Vidiko organisation, the successor organisation of VMAFx.
VMAFx stays the scoring library and its tools. The new repository is
[vidiko/torcul](https://github.com/vidiko/torcul). Its name comes from Latin
*torcular*, a press (wine or oil): input goes in, a pressed product comes out.
It is said TOR-kul.

## Decision

1. The encode service, its job API and its eight slices live in
   vidiko/torcul. This ADR supersedes ADR-3378; that record stays as the design
   history of the service.
2. VMAFx adds no encode-job code to `api/vmafx-platform.toml`, `cmd/vmafx-node`
   or `tools/vmaf-tune`. Its scoring, `vmaf-tune auto` and compare surfaces
   remain and are what torcul calls.
3. torcul uses VMAFx in process through the `libvmafx.so.1` C ABI from Rust
   (torcul's ADR-0001). VMAFx owes it the Rust crate over `libvmafx.so.1`
   generated from `core/api/vmafx.toml` (#2320). Until then torcul may call
   the `vmaf` CLI.
4. Issues #2986 to #2993 are closed in VMAFx with a link to their copy in
   torcul (vidiko/torcul#2 to vidiko/torcul#9). GitHub cannot transfer issues
   between owners.

## Alternatives considered

| Option | For | Against | Verdict |
| :--- | :--- | :--- | :--- |
| Keep the service in VMAFx (ADR-3378) | One repository, no cross-repository API | Couples a scoring library's release cadence to an encode service; mixes FFmpeg encode policy into the scoring tree | Rejected by the operator |
| Own repository in Vidiko, VMAFx called in process | Separate cadence; low-level parts become reusable Rust crates; per-trial overhead about 0.05 ms against about 4 ms for a CLI call (measured, torcul ADR-0001) | Waits on the `libvmafx.so.1` Rust crate | Chosen |
| Own repository, VMAFx called through the `vmafx-node` gRPC API | No native link | `Score` spawns the CLI per call; `ScoreStream` is CPU-only and ships raw planes; GPU ownership split between two daemons | Rejected (torcul ADR-0001) |

## Consequences

- **Positive**: VMAFx keeps its scope (scoring, tools, bindings); the encode
  service evolves on its own cadence.
- **Negative**: the encode code VMAFx already has in Go (`pkg/hwdevice`,
  `pkg/ffencode`, `pkg/codecadapter`, `pkg/hdr`, `pkg/tune/auto`) exists twice
  while torcul ports it; once torcul reaches parity, VMAFx either uses its
  crates or retires its copies.
- **Neutral**: the 0.10.0 roadmap row names torcul instead of the slices.

## References

- Operator decision 2026-10-27 (paraphrased): the video encode service leaves
  VMAFx and becomes its own repository in Vidiko; VMAFx stays the scoring
  library and tools. Ledger `vidiko-encode-service-placement-20261027`.
- Repository name torcul, public (paraphrased popup answer). Ledger
  `vmafx-encode-service-name-r2-20261027-answer`.
- [ADR-3378](3378-encode-job-api.md), superseded.
- torcul ADR-0001, `docs/adr/0001-vmafx-integration.md` in vidiko/torcul.
