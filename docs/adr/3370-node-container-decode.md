<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3370: The node decodes container inputs through ffmpeg into a Y4M pipe

- **Status**: Accepted
- **Date**: 2026-10-24
- **Deciders**: Lusoris (operator order vmafx-node-container-decode-20261024)
- **Tags**: node, server, ffmpeg, platform

## Context

A media library holds containers (`.mkv`, `.mp4`), not Y4M clips. The node's
scorer reads only Y4M, so every library job failed before scoring, and a short
sample of a long clip needed the whole clip decoded first. The node image
already carries ffmpeg.

## Decision

We will decode every input that is not `.y4m` or `.yuv`, and every input with
a time window, with the node's ffmpeg (`VMAFX_FFMPEG_BIN`, default `ffmpeg`)
into a Y4M pipe that `libvmaf.Scorer.ScoreReaders` reads (`pkg/y4mdecode`).
Nothing is written to disk. `ScoringParams` gains `window_start` and
`window_duration` (seconds as decimal strings), passed to ffmpeg as `-ss` and
`-t`. The scoring-root check runs on the source path before any decode, so an
input outside the roots, or a traversal out of them, is refused as before. A
file that is not media fails the job with ffmpeg's error (capped at 4 KiB); a
window past the end fails with `no frame`; a zero or negative duration is
refused. A raw `.yuv` cannot be decoded or windowed and is refused.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Y4M pipe from ffmpeg (chosen) | No disk use; window decodes only the sample; one decoder already in the image | ffmpeg process per input | — |
| Decode to a bounded temp file | Seekable input | Disk space per job, cleanup on failure, a size limit to tune | The pipe needs neither |
| libav* through cgo | No child process | New cgo surface and licence review; the image already ships ffmpeg | Larger change for the same result |
| Require Y4M sources | No code | Every library input fails | Does not meet the order |

## Consequences

- Library samples score without a full decode; a 10 s window decodes 10 s.
- The node depends on ffmpeg at run time for containers; without it a
  container job fails naming the binary.
- Guards: `pkg/y4mdecode/y4mdecode_test.go`,
  `cmd/vmafx-node/executor_container_test.go` (H.264 `.mkv`, HEVC `.mp4`,
  non-media, window past end, zero duration, outside root, traversal).

## References

- Operator order vmafx-node-container-decode-20261024 (paraphrased): the node
  accepts container inputs, decodes them with the baked ffmpeg, takes an
  optional start and duration, and keeps the scoring-root check on the source.
- [ADR-1526](1526-node-storage-streamed-inputs.md) (streamed inputs).
