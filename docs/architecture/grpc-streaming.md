<!-- markdownlint-disable MD060 -->
# gRPC streaming (`ScoreStream`) — Phase 1

Status: **Proposed** (ADR-0933). Phase 1 ships the schema + server stub.
Per-frame scoring is wired in Phase 2.

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
| 1 | Proto schema + regenerated Go bindings + server handler stub that validates framing and returns `codes.Unimplemented` + client wrapper in `pkg/score` + smoke tests + this doc. | **This PR (ADR-0933)**. |
| 2 | Wire the handler to `pkg/libvmaf` via a new in-memory picture-import path that takes raw planar bytes instead of a file path. Per-frame scoring becomes real; `AggregateScore` returns the pooled VMAF. | Tracked under ADR-0933 follow-up. |
| 3 | Benchmarks vs. path-unary; tune `max-recv-msg-size` and stream window sizes; flip the unary `Score` handler to internally delegate to `ScoreStream` for the single-file case (network surface unchanged). | Tracked under ADR-0933 follow-up. |

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
# From a fresh clone:
buf generate proto                  # regen Go bindings from vmafx.proto
go build ./...                      # compiles the new server + client surface
go test ./pkg/score/...             # exercises framing validation end-to-end
```

The smoke tests in [`pkg/score/grpc_client_test.go`](../../pkg/score/grpc_client_test.go)
spin up an in-process gRPC server that mimics the Phase 1 stub, dial it,
and confirm:

- A `StreamConfig` with zero width / height is rejected with
  `codes.InvalidArgument`.
- A valid `StreamConfig` is accepted but the server replies with
  `codes.Unimplemented` (Phase 2 will replace this with real per-frame
  scoring).

## References

- [ADR-0933](../adr/0933-grpc-streaming-multi-frame-scoring.md) —
  decision record for this rollout.
- [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) — original vmafx-server
  unary surface.
- [ADR-0711](../adr/0711-vmafx-controller-impl.md) — controller that
  will consume `ScoreStream` directly.
