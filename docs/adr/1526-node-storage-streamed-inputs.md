<!-- markdownlint-disable MD013 MD060 -->
# ADR-1526: vmafx-node reads job sources through pkg/storage and streams http-served inputs into the vmaf CLI through pipes

- **Status**: Accepted (partially superseded by [ADR-2350](2350-cloud-native-platform.md) for rclone as the default input path (presigned object URLs by default, rclone opt-in))
- **Date**: 2026-10-04
- **Deciders**: maintainer, agent
- **Tags**: go, node, storage, rclone, helm, phase4b, fork-local

## Context

[ADR-0719](0719-vmafx-node-rclone-integration.md) chose rclone for remote
sources: `rclone serve http` as the primary mode, `rclone mount` as the
fallback, both behind `pkg/storage`. Nothing called `pkg/storage`: the node's
executor handed a job's `reference` and `distorted` to the vmaf CLI as they
were, so only local paths worked. The Helm chart offered
`storage.mode: http-serve | rclone`, where `rclone` matched no mode of the
package, and the package's `New` turned an unknown or `auto` mode into
http-serve without saying so (docs audit of 2026-10-03, defect 12).

ADR-0719 wrote that ffmpeg would read the http-serve URL. The node does not
run ffmpeg on a scoring job; the vmaf CLI does the reading, and it opens files,
not URLs.

## Decision

- The executor prepares both sources with the configured `pkg/storage` mode
  (`VMAFX_STORAGE_MODE`, default `auto`) and releases the rclone processes and
  mounts after the job. Two paths go to the CLI as files. When either input is
  an http(s) URL (an `rclone serve http` instance, or a source that already is
  a URL), both are streamed into the CLI: `libvmaf.Scorer.ScoreReaders` passes
  the read ends of two pipes as descriptors 3 and 4 (`-r /dev/fd/3
  -d /dev/fd/4`). Nothing is written to the node's disk.
- A stream that fails while it is read fails the job even when the CLI
  returned a score: the CLI cannot tell a broken stream from the end of a
  clip, and it scores a shorter distorted clip with exit status 0.
- `storage.Open` replaces `New` for callers that need validation: it accepts
  `http-serve`, `mount` and `auto` only, resolves `auto` to mount when
  `/dev/fuse` and `fusermount3`/`fusermount` exist and to http-serve otherwise,
  logs the choice, and refuses mount on a host without FUSE. `New` stays,
  deprecated (HISS-14). Local paths and http(s) URLs need no rclone in any
  mode; mount points go under `VMAFX_STORAGE_MOUNT_ROOT`.
- An unknown mode stops the node at startup; the chart's schema accepts
  `http-serve | mount | auto` and refuses the old `rclone`. The chart sets
  `VMAFX_RCLONE_CONFIG` only when it mounts the rclone Secret.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Named pipes (FIFOs) in a per-job directory | No change to the scorer API | A writer blocks in `open` until the CLI opens the FIFO; a CLI that fails before opening the second input leaves a goroutine blocked, and teardown needs extra opens | Inherited pipe descriptors close with the child; no filesystem node |
| Download to a temp file, then score | Simplest | Materialises every source on the node's disk, the cost ADR-0719 exists to avoid | Not chosen |
| Decode with ffmpeg into the CLI | Would accept encoded sources (MP4, MKV) | A second process per input, a change of what a scoring job accepts (today Y4M), a new failure surface | Out of scope; the CLI reads Y4M directly |
| Keep `New` and make it strict | One constructor | Changes the behaviour of an exported function under its callers | `Open` added, `New` deprecated |
| Map the chart's `rclone` to `auto` | Old values files keep rendering | A setting that never worked would silently become another mode | Refused with the allowed values |

## Consequences

- **Positive**: jobs on rclone remotes and http(s) URLs score;
  `TestEndToEndControllerNodeRcloneSources` scores one job through each mode
  against a real controller and rclone with the CLI's file score.
- **Positive**: a truncated stream fails the job instead of scoring the frames
  it delivered (`TestScoreReaders_StreamBrokenAtFrameBoundaryFails`).
- **Negative**: streaming needs a Unix host (inherited descriptors and
  `/dev/fd`); elsewhere `ScoreReaders` returns
  `ErrStreamInputsUnsupported`.
- **Neutral / follow-ups**: the published node image ships rclone but no FUSE
  helper, so mount mode is refused there and `auto` resolves to http-serve. A
  Helm values file with `storage.mode: rclone` no longer renders.

## References

- [ADR-0719](0719-vmafx-node-rclone-integration.md), [ADR-1524](1524-vmafx-node-controller-client.md).
- Docs audit defect list of 2026-10-03, defect 12.
- Popup answer of 2026-10-04, "Unwired distributed-platform features": "Implement them now".
