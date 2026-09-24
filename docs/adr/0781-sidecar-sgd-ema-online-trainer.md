<!-- markdownlint-disable MD013 MD060 -->
# ADR-0781: Sidecar online training — SGD + EMA + replay buffer

- **Status**: Proposed
- **Date**: 2026-05-29
- **Deciders**: Lusoris
- **Tags**: ai, sidecar, online-learning, k8s, vmafx-node, phase4b, fork-local

## Context

VMAFX Phase 4b (ADR-0709) proposes converting the platform into a distributed
cloud-native scoring service. Its online-learning design would turn per-job
features and ground-truth scores into feedback samples. The current scoring and
executor paths do not construct or send that feedback envelope, so this signal
is not connected to the trainer today.

Research-0733 evaluated three architectures for closing the
encode-score-train loop:

- **Option A** — Python sidecar container co-located in the same pod
- **Option B** — Dedicated training-node pool receiving batched triples
  from the controller
- **Option C** — Pure-Go SGD (Gorgonia or hand-rolled)

The research digest recommended **Option A** for v1.  Option B
over-engineers at current cluster scale (< 1000 triples/hour).  Option C
is blocked by the Go ML ecosystem's immaturity for multi-layer networks.

The user direction that motivates this work (per ADR-0709 references):
continual model improvement during live encoding without waiting for the
next offline batch-training round-trip.

The existing `tools/vmaf-tune/sidecar.py` (ADR-0394) implements on-host
online ridge regression for the vmaf-tune predictor surface — a related
but separate concern (single-host bias correction, no k8s, no ONNX
export).  This ADR covers the distributed k8s sidecar that fine-tunes the
tiny-AI ONNX models consumed by libvmaf's DNN path.

## Decision

The proposed end state is a Python sidecar container (`ai/sidecar/`) co-located
with each `vmafx-node` pod and connected through a Unix domain socket at
`/tmp/vmafx-sidecar.sock` on a shared `emptyDir` volume.

This ADR remains **Proposed**. The repository implements standalone trainer and
transport building blocks, but not the end-to-end producer, deployment, or
checkpoint-consumption path. The statements below distinguish those implemented
building blocks from the proposed product topology.

The standalone Python sidecar implements:

1. **Replay buffer** (`replay_buffer.py`) — bounded ring buffer, capacity
   10 000 samples (FIFO eviction).  Chosen because 10 000 × 80 float32
   features ≈ 3.2 MB in-process; enough to represent ~200 hours of a
   50-encode/hour workload without unbounded growth.
2. **Online SGD + EMA** (`sgd_ema.py`) — SGD with momentum (default) or
   Adam; EMA shadow with `beta=0.999`; gradient clipping at `max_norm=1.0`;
   100-step linear LR warmup.  EMA decay `0.999` follows the Mean Teacher
   paper (Tarvainen & Valpola, 2017) and is the standard for online VMAF
   work (Research-0733 §3.3).
3. **Checkpoint export** — EMA model exported to ONNX (opset 17, matching
   ADR-0249) with an atomic rename + SHA-256 sidecar file.  Dual-condition
   trigger: `>= 10 min` AND `>= 1000 new samples` since last checkpoint.
4. **Socket server** (`online_trainer.py`) — newline-delimited JSON,
   one-thread-per-connection.

The Go transport (`cmd/vmafx-node/online_feedback.go`) separately implements a
non-blocking `FeedbackClient.Send()` queue with a 1000-entry capacity and a
background drainer. Its lifecycle provider exists, but no scoring or executor
production path calls `Send()`. The encode-score-train loop is therefore open;
the client counters remain transport diagnostics rather than live product
metrics.

`deploy/helm/vmafx/templates/sidecar-trainer.yaml` contains an orphaned proposed
named template. The current `templates/node.yaml` does not include it, its
recorded `node-deployment.yaml` consumer no longer exists, and
`values.schema.json` rejects `sidecar.*`. The chart therefore does not render or
support this topology. [ADR-1309](1309-socket-path-ownership-and-owner-only-mode.md)
records the resulting same-UID, owner-only standalone socket contract until real
deployment wiring is designed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Python sidecar per pod (proposed) | Reuses `ai/` PyTorch stack. Could provide low data-to-training latency and pod-level process isolation. | Would require a larger image, real chart wiring, a producer call site, checkpoint consumption, and lifecycle ownership. | Selected design direction, not yet an implemented deployment. |
| Dedicated training-node pool (Research-0733 Option B) | Clean separation of concerns. Independently scaled. Smaller scoring pod images. | Cross-pod data transport (1–10 s RTT). Requires a fault-tolerant training queue. Two new node types in Helm + Operator. Overkill at v1 scale. | Deferred to v2 when cluster scale demands dedicated GPU training pools. |
| Pure-Go SGD (Research-0733 Option C) | Single binary. No Python dep. Lowest per-sample overhead. | Go ML ecosystem immature for multi-layer nets. Gorgonia effectively unmaintained. Cannot load ONNX natively. Blocks future improvements (attention, LoRA). | Not viable for full neural network training at any scale. |
| gRPC loopback instead of Unix socket | Strong typing. Schema evolution. Native flow control. | Second RPC framework in the pod. Extra container port declaration. Measurably higher per-message overhead on loopback than UDS for < 1 KB payloads. | Unix socket + newline-delimited JSON is simpler, lower overhead, and sufficient for the message rate (< 100/s). The Research-0733 preference for gRPC is overridden by the user's implementation spec. |
| Shared-volume file watch | No socket needed. | Worst latency (seconds for inotify). Not suitable for near-real-time training. | Rejected. |

## Expected consequences if integrated

These are design consequences, not claims about the currently shipped scoring
flow.

- **Positive**:
  - The encode-score-train loop could close to minutes instead of hours/days.
  - A future producer can use the implemented non-blocking
    `FeedbackClient.Send()` queue and graceful drop under back-pressure.
  - EMA stabilises checkpoints against noisy gradient updates from
    short-content bursts (e.g., an hour of HDR animation encodes).
  - Replay buffer prevents catastrophic forgetting when content distribution
    shifts.
  - A correctly wired sidecar could fail independently of the scoring process.
- **Negative**:
  - Pod image grows by ~1.5 GB (CPU PyTorch) or ~5 GB (CUDA PyTorch).
  - Resource contention would need a deliberate device policy. The current
    trainer is CPU-only and exposes no `VMAFX_SIDECAR_CUDA` setting.
  - A future deployment would add per-pod checkpoint state that an operator or
    another controller must manage.
- **Neutral / follow-ups**:
  - The automatic feedback producer, supported Helm values and workload wiring,
    shared socket mount, and checkpoint verification/loading are not
    implemented. `VmafxModelTraining` types exist, but they do not deploy or
    observe this Unix-socket trainer.
  - Stability gate (PLCC regression check before marking a checkpoint
    `latest-stable`) is a follow-up per Research-0733 §3.4.
  - EWC regularisation deferred to v2 (requires Fisher information matrix
    from the base corpus — not available at sidecar startup).
  - LoRA-style per-tenant adapters deferred to v2.

## References

- [Research-0733 — VMAFX sidecar online training architecture](../research/0733-vmafx-sidecar-training-architecture.md)
- [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) — Phase 4b umbrella, item 4b.7
- [ADR-0394](0394-local-sidecar-training.md) — local sidecar for vmaf-tune predictor (separate surface)
- [ADR-0249](0249-fr-regressor-v1.md) — ONNX export opset constraints
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI per-PR docs rule
- [ADR-1309](1309-socket-path-ownership-and-owner-only-mode.md) — current owner-only socket and pathname-lifecycle contract
- Tarvainen & Valpola (2017) "Mean teachers are better role models" — EMA decay `beta=0.999`
- Source: `req` — user direction to implement sidecar online training per Research-0733
  (Python sidecar, SGD + EMA + replay buffer, Phase 4b distributed platform piece).
