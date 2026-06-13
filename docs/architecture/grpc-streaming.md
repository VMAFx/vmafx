<!-- markdownlint-disable MD060 -->
# gRPC streaming (`ScoreStream`)

Status: **Accepted / implemented** (ADR-0933). Phase 1 shipped the schema +
server stub; Phase 2 (2026-06-13) wired the handler to the libvmaf engine via
the in-memory `pkg/libvmaf.StreamScorer`. The RPC now returns real per-frame
scores. Both `vmafx-server` and `vmafx-node` ([ADR-1109](../adr/1109-vmafx-node-serve-scoring-grpc.md))
serve it.

## Why a streaming RPC

The original `vmafx-server` gRPC service exposes a single unary scoring
RPC, `Score(ScoreRequest) -> ScoreResponse`, where `ScoreRequest` is
path-based: the client passes absolute filesystem paths to a reference
and a distorted clip on shared storage, and the server opens both,
runs libvmaf to EOF, and returns one pooled score.

That shape does not cover two real workloads:

1. **Live encode quality monitoring.** A transcoder produces frames as
   it goes. The client wants per-frame VMAF *while the encode is still
   running*, not one pooled number after the file is final.
2. **In-memory raw pixels without disk inflation.** When the client
   already has the pixels in RAM (an ffmpeg sidecar reading frames from
   `stdout`, an MCP tool, a Rust test harness), shoving them through a
   single unary `ScoreRequest` means a multi-GB request payload — gRPC's
   default `max-recv-msg-size` is 4 MiB and even with that lifted, a
   single jumbo request destroys flow control and blocks the server's
   HEAD-of-line queue.

The new RPC is bidirectional streaming:

```proto
rpc ScoreStream(stream ScoreStreamRequest) returns (stream ScoreStreamResponse);
```

Both directions are streams so the client can begin consuming per-frame
scores while it is still pushing later frames; gRPC handles backpressure
in both directions automatically.

## Message shape

The proto lives in [`proto/vmafx.proto`](../../proto/vmafx.proto).
Summarised:

| Direction | Message | Meaning |
|---|---|---|
| client -> server | `ScoreStreamRequest{ config: StreamConfig }` | Exactly once, first. Declares `width`, `height`, `pixel_format`, optional `model`, optional `frame_count_hint`. |
| client -> server | `ScoreStreamRequest{ frame_pair: FramePair }` | Repeated. `frame_index` strictly monotonic from 0. `raw_reference` / `raw_distorted` are planar Y/U/V bytes in the declared pixel format. |
| server -> client | `ScoreStreamResponse{ frame_score: FrameScore }` | One per processed frame. `frame_index`, `score`, per-feature map. |
| server -> client | `ScoreStreamResponse{ aggregate: AggregateScore }` | Exactly once, last, after the client half-closes. Pooled VMAF, per-feature pool, frame count, elapsed wall time. |

Supported pixel formats (`PixelFormat`): YUV 4:2:0, 4:2:2, 4:4:4 in
8-bit and 10-bit-little-endian — a subset of FFmpeg's `AVPixelFormat`
that libvmaf already accepts on its picture-import path.

## Backwards compatibility

The unary `Score` and `Health` RPCs are unchanged. The proto package
stays `vmafx.v1`. All existing clients keep working without recompiling.

Deprecation of unary `Score` is staged for the next major version,
after Phase 3 lands. The follow-up ADR will document the timeline.

## Phase rollout

| Phase | Surface | Status |
|---|---|---|
| 1 | Proto schema + regenerated Go bindings + server handler stub that validates framing and returns `codes.Unimplemented` + client wrapper in `pkg/score` + smoke tests + this doc. | **Done** (ADR-0933). |
| 2 | Wire the handler to `pkg/libvmaf` via the in-memory `StreamScorer` picture-import path that takes raw planar bytes instead of a file path. Per-frame scoring is real; `AggregateScore` returns the pooled VMAF. `vmafx-node` also serves the RPC (ADR-1109). | **Done** (2026-06-13). |
| 3 | Benchmarks vs. path-unary; tune `max-recv-msg-size` and stream window sizes; flip the unary `Score` handler to internally delegate to `ScoreStream` for the single-file case (network surface unchanged). | Tracked under ADR-0933 follow-up. |

### How per-frame scoring works

Several VMAF features (notably motion) are temporal — they only finalise once
the whole sequence has been read and flushed. The handler therefore ingests
every `FramePair` first, and once the client half-closes it flushes the engine
and harvests the per-frame scores via `vmaf_score_at_index`, streaming back the
`FrameScore` messages followed by the terminal `AggregateScore`. gRPC flow
control still bounds the request side while frames are pushed, so a multi-GB
sequence does not inflate a single message. The streaming pooled VMAF is
bit-identical to the file-reading `ScoreDirect` path (verified on the 48-frame
golden pair).

## Client usage

The `pkg/score` package wraps the generated client so callers do not
have to hand-craft the `oneof` framing:

```go
package main

import (
    "context"
    "errors"
    "io"
    "log"

    vmafxv1 "github.com/VMAFx/vmafx/gen/go"
    "github.com/VMAFx/vmafx/pkg/score"
)

func main() {
    cli, err := score.Dial("vmafx-server:50051")
    if err != nil {
        log.Fatal(err)
    }
    defer cli.Close()

    ctx := context.Background()
    stream, err := cli.OpenScoreStream(
        ctx,
        1920, 1080,
        vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P,
        "vmaf_v0.6.1",
        0, // frame_count_hint: unknown
    )
    if err != nil {
        log.Fatal(err)
    }

    // Push frames from your decoder loop ...
    for i := uint32(0); i < numFrames; i++ {
        ref, dist := nextFramePair()
        if err := stream.PushFrame(i, ref, dist); err != nil {
            log.Fatal(err)
        }
    }
    if err := stream.CloseSend(); err != nil {
        log.Fatal(err)
    }

    // Drain per-frame scores + terminal aggregate.
    for {
        fs, agg, err := stream.Recv()
        if errors.Is(err, io.EOF) {
            break
        }
        if err != nil {
            log.Fatal(err)
        }
        if agg != nil {
            log.Printf("pooled VMAF: %.4f over %d frames in %dms",
                agg.Score, agg.FramesProcessed, agg.ElapsedMs)
            continue
        }
        log.Printf("frame %d: %.4f", fs.Index, fs.Score)
    }
}
```

## Reproducer / smoke test

```bash
# From a fresh clone, with a CPU libvmaf build at core/build-cpu:
meson setup core/build-cpu core -Denable_cuda=false -Denable_sycl=false
ninja -C core/build-cpu src/libvmaf.so.3.0.0

# End-to-end streaming scoring against the 48-frame golden pair:
CGO_ENABLED=1 go test ./pkg/libvmaf/ -run StreamScorer            # engine
CGO_ENABLED=1 go test ./cmd/vmafx-server/ -run ScoreStream        # server RPC
CGO_ENABLED=1 go test ./cmd/vmafx-node/server/ -run ScoreStream   # node RPC
```

The end-to-end tests push the real 576×324 / 48-frame YUV fixtures over an
in-process gRPC stream and assert the server returns one `FrameScore` per frame
plus a terminal `AggregateScore` whose pooled VMAF matches `ScoreDirect`.
Validation-only cases (zero dimensions, non-config first message, frame-size
mismatch, unloadable model) assert the corresponding `codes.InvalidArgument` /
`codes.NotFound` without needing the fixtures.

## References

- [ADR-0933](../adr/0933-grpc-streaming-multi-frame-scoring.md) —
  decision record for this rollout.
- [ADR-1109](../adr/1109-vmafx-node-serve-scoring-grpc.md) — vmafx-node serves
  the same `VmafxScoring` surface.
- [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) — original vmafx-server
  unary surface.
- [ADR-0711](../adr/0711-vmafx-controller-impl.md) — controller that
  will consume `ScoreStream` directly.
