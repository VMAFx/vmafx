<!-- markdownlint-disable MD060 -->
# ADR-0933: gRPC streaming for multi-frame scoring (`ScoreStream`)

- **Status**: Accepted
- **Date**: 2026-05-31 (Phase 1); accepted 2026-06-13 (Phase 2 implemented)
- **Deciders**: lusoris, Claude (modernization #18)
- **Tags**: grpc, server, api, streaming, fork-local

> **Phase 2 update (2026-06-13)**: the design below was implemented as
> specified — the bidirectional `ScoreStream` handler is wired to the libvmaf
> engine via the new in-memory `pkg/libvmaf.StreamScorer` (mirrors
> `vmaf_picture_alloc` + `vmaf_read_pictures`), and both `vmafx-server`
> (`cmd/vmafx-server/grpc_server.go`) and `vmafx-node`
> (`cmd/vmafx-node/server/server.go`, ADR-1109) serve it. Per-frame scores are
> harvested after the client half-closes (temporal features such as motion only
> finalise at flush), then streamed back as N `FrameScore` messages followed by
> the terminal `AggregateScore`. The streaming pooled VMAF is bit-identical to
> the file-reading `ScoreDirect` path on the 48-frame golden pair. This ADR
> flips from Proposed to Accepted because the implementation matches the design.

## Context

`vmafx-server` currently exposes a single unary scoring RPC:

```proto
rpc Score(ScoreRequest) returns (ScoreResponse);
```

`ScoreRequest` is path-based — the client hands the server an absolute path to
a reference and a distorted file on shared storage, the server opens both,
runs libvmaf to EOF, and returns one `ScoreResponse` with the pooled score.

Two real workloads do not fit that shape:

1. **Live encode quality monitoring.** A transcoder pipeline produces frames
   on-the-fly. The client wants per-frame VMAF as the encode progresses, not
   one pooled score after the whole clip is on disk. Path-based unary forces
   the client to materialize a finished file first, which a streaming encode
   doesn't have.

2. **Large-clip request inflation.** When the client *does* have raw pixels
   in memory (e.g. an `ffmpeg`-driven sidecar that reads frames from stdout),
   shoving them through a single unary call means a multi-GB
   `ScoreRequest` payload. gRPC's default `max-recv-msg-size` is 4 MiB; even
   bumped to 1 GiB the all-at-once payload pattern destroys flow control and
   blocks the server's HEAD-of-line queue.

Both workloads want the same primitive: a long-lived bidirectional channel
where the client pushes frame pairs as they become available and the server
emits per-frame scores as they're computed, with gRPC handling backpressure.

`vmaf-tune`, the new `vmafx-controller` (ADR-0711), the upcoming MCP
multi-frame scoring tool, and the Netflix-style encoder-search backlog all
hit one or both of these workloads.

## Decision

Add a third RPC to `service VmafxScoring`:

```proto
rpc ScoreStream(stream ScoreStreamRequest) returns (stream ScoreStreamResponse);
```

The bidirectional shape is deliberate (rather than client-streaming-only):

- Client side carries a leading `StreamConfig` (width, height, pixel format,
  optional model, optional frame-count hint) followed by N `FramePair`
  messages, each with monotonically increasing `frame_index` and the raw
  planar Y/U/V bytes for both reference and distorted frames.
- Server side returns N `FrameScore` messages multiplexed with a single
  terminal `AggregateScore` on a oneof-typed `ScoreStreamResponse`. The
  client can begin consuming per-frame scores while it is still pushing
  later frames — gRPC flow control handles backpressure in both directions.

The v1 unary `Score(ScoreRequest)` RPC is preserved unchanged. The new
streaming surface is additive — the proto stays `vmafx.v1`, no breaking
change. Deprecation of `Score` is staged for the next major version
(follow-up ADR, post-GA).

Phase 1 (this PR) ships the proto schema, regenerated Go bindings, the
server handler stub (returns `codes.Unimplemented`), the client wrapper
that demonstrates how to consume the streaming surface, and the
architecture doc. Phase 2 will wire the handler to libvmaf via the
existing `pkg/libvmaf` scorer plus a new picture-import path that takes
raw bytes instead of a file path. Phase 3 will add benchmarks and flip
the unary `Score` to internally delegate to `ScoreStream` for the
single-file case (with the network surface unchanged).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Bidirectional `stream → stream` (chosen)** | Per-frame latency, gRPC flow control in both directions, terminal aggregate fits naturally on the response side, future-proof for HDR / variable frame rate. | Schema is more complex (`oneof` request + `oneof` response). | This is the only shape that covers both live-encode monitoring and large-clip-without-inflation. |
| Server-streaming only (unary request -> stream response) | Simpler schema, request carries paths. | Doesn't solve the in-memory raw-pixel case; still needs huge upload of frames before scoring begins. | Half the motivating workloads (live monitoring) are unsolved. |
| Client-streaming only (stream request -> unary response) | Solves the upload case. | Loses per-frame visibility — server has to wait for client EOF to return any score. | Live encode monitoring needs per-frame results during the stream, not at EOF. |
| Multiple unary calls (`Score(window=[start, end])`) | Trivial schema, no new RPC. | Per-call overhead destroys throughput; client has to chunk windows manually; aggregate pooling lives in the client. | Reinvents streaming poorly. |
| HTTP/2 raw bytes outside of proto | No proto codegen pain. | Throws away typed schema, validation, the v1 generated client. | Doesn't fit the rest of the Go service surface. |
| Bump to `vmafx.v2` package immediately | Clean break, lets us deprecate `Score` now. | Doubles the surface area, breaks every existing client overnight, and the v2-only methods would carry no extra meaning today. | Premature; reserve `vmafx.v2` for the actual breaking-rename in the major release where unary `Score` retires. |

## Consequences

- **Positive**:
  - `vmafx-controller` (ADR-0711) gets the streaming primitive it needs to
    parallelize encode-and-score pipelines without disk round-trips.
  - Live-encode monitoring becomes a first-class workload, not a hack.
  - MCP can expose a multi-frame scoring tool that doesn't materialise YUV
    on disk before scoring.
  - Backpressure and cancellation come for free from gRPC.
- **Negative**:
  - Two scoring code paths in the server (unary + streaming) until Phase 3
    consolidates by having unary delegate internally. Duplicated test
    surface area in the interim.
  - `oneof` requests need careful validation (config must arrive first,
    `frame_index` must be strictly monotonic). The Phase 2 handler must
    reject malformed sequences with `InvalidArgument`.
- **Neutral / follow-ups**:
  - Phase 2: wire `ScoreStream` to `pkg/libvmaf` via a new picture-import
    path that accepts in-memory planar bytes (mirrors libvmaf's existing
    `vmaf_picture_alloc` + `vmaf_read_pictures`).
  - Phase 3: benchmarks comparing path-unary vs. raw-stream throughput on
    1080p and 4k; tune the gRPC `max-recv-msg-size` and per-stream window
    sizes accordingly.
  - Major-version deprecation ADR for unary `Score` once Phase 3 lands.

## References

- See [ADR-0703](0703-vmafx-server-go-grpc.md) for the original
  vmafx-server unary surface.
- See [ADR-0711](0711-vmafx-controller-impl.md) for the controller that
  consumes this streaming RPC.
- See [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) for the
  distributed-platform context where multi-frame scoring is a hot path.
- Source: req — modernization backlog item #18 ("gRPC streaming for
  multi-frame scoring"), per user direction 2026-05-31.
