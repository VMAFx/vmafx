<!-- markdownlint-disable MD013 MD060 -->
# ADR-0781: Sidecar online training — SGD + EMA + replay buffer

- **Status**: Proposed
- **Date**: 2026-05-29
- **Deciders**: Lusoris, Claude
- **Tags**: ai, sidecar, online-learning, k8s, vmafx-node, phase4b, fork-local

## Context

VMAFX Phase 4b (ADR-0709) converts the platform into a distributed
cloud-native scoring service.  Every job that `vmafx-node` executes
produces a `(features, predicted_score)` pair alongside the ground-truth
VMAF score from libvmaf.  That pair is a free training signal that is
currently discarded after each job.

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

We will ship a Python sidecar container (`ai/sidecar/`) that runs
co-located with each `vmafx-node` pod, connected via a Unix domain socket
at `/tmp/vmafx-sidecar.sock` (shared `emptyDir` volume).

The sidecar implements:

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
   one-thread-per-connection.  Non-blocking from the Go side: `FeedbackClient`
   enqueues into a 1000-entry ring buffer; a background goroutine drains it.

The Go-side client (`cmd/vmafx-node/online_feedback.go`) calls
`FeedbackClient.Send()` from the scoring path after `scorer.Score()` returns.
The call is non-blocking; dropped messages are counted via
`FeedbackClient.Dropped()`.

The Helm sidecar container spec is rendered by
`deploy/helm/vmafx/templates/sidecar-trainer.yaml` via the
`vmafx.sidecarContainer` named template and is gated by
`.Values.sidecar.trainer.enabled` (default `false`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Python sidecar per pod (chosen) | Reuses `ai/` PyTorch stack. Sub-100 ms data-to-training latency. Pod-level isolation from scoring. Resource limits declared separately. | Heavier pod image (~1.5 GB PyTorch CPU / ~5 GB CUDA). Sidecar lifecycle managed by Operator. | — |
| Dedicated training-node pool (Research-0733 Option B) | Clean separation of concerns. Independently scaled. Smaller scoring pod images. | Cross-pod data transport (1–10 s RTT). Requires a fault-tolerant training queue. Two new node types in Helm + Operator. Overkill at v1 scale. | Deferred to v2 when cluster scale demands dedicated GPU training pools. |
| Pure-Go SGD (Research-0733 Option C) | Single binary. No Python dep. Lowest per-sample overhead. | Go ML ecosystem immature for multi-layer nets. Gorgonia effectively unmaintained. Cannot load ONNX natively. Blocks future improvements (attention, LoRA). | Not viable for full neural network training at any scale. |
| gRPC loopback instead of Unix socket | Strong typing. Schema evolution. Native flow control. | Second RPC framework in the pod. Extra container port declaration. Measurably higher per-message overhead on loopback than UDS for < 1 KB payloads. | Unix socket + newline-delimited JSON is simpler, lower overhead, and sufficient for the message rate (< 100/s). The Research-0733 preference for gRPC is overridden by the user's implementation spec. |
| Shared-volume file watch | No socket needed. | Worst latency (seconds for inotify). Not suitable for near-real-time training. | Rejected. |

## Consequences

- **Positive**:
  - Encode-score-train loop closes to minutes instead of hours/days.
  - The Go scoring path is never blocked: `FeedbackClient.Send()` is
    non-blocking with graceful drop under back-pressure.
  - EMA stabilises checkpoints against noisy gradient updates from
    short-content bursts (e.g., an hour of HDR animation encodes).
  - Replay buffer prevents catastrophic forgetting when content distribution
    shifts.
  - Sidecar container failure does not affect the scoring container (separate
    process, different restart policy).
- **Negative**:
  - Pod image grows by ~1.5 GB (CPU PyTorch) or ~5 GB (CUDA PyTorch).
  - Resource contention if both containers attempt to use the same CUDA
    device simultaneously.  Mitigation: sidecar trains on CPU (default);
    CUDA training enabled only via `VMAFX_SIDECAR_CUDA=1` when the node
    pod has excess GPU budget.
  - New per-pod state (checkpoint directory mount) that must be managed
    by the operator.
- **Neutral / follow-ups**:
  - `VmafxModelTraining` CRD + operator reconciler (Research-0733 §4) are
    not in scope for this PR.  The Helm values gate (`sidecar.trainer.enabled`)
    and the `FeedbackClient` wiring in `vmafx-node` are the v1 surface;
    the Operator is a separate ADR.
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
- Tarvainen & Valpola (2017) "Mean teachers are better role models" — EMA decay `beta=0.999`
- Source: `req` — user direction to implement sidecar online training per Research-0733
  (Python sidecar, SGD + EMA + replay buffer, Phase 4b distributed platform piece).
