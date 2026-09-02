### Research-0733: VMAFX Sidecar Online Training Architecture

Added `docs/research/0733-vmafx-sidecar-training-architecture.md` — a comprehensive
architecture research digest for the Phase 4b.7 sidecar online training workstream
(ADR-0709). Covers: evaluation of three architecture options (Python sidecar per node,
dedicated training nodes, Go-native online learning); detailed data-flow design including
triple capture, gRPC transport, EMA-SGD training loop with replay-buffer catastrophic-
forgetting mitigation, and atomic checkpoint swap; `VmafxModelTraining` CRD definition with
full field documentation; operator reconcile loop pseudocode; and open-question analysis
covering training data persistence, multi-tenant isolation, and below-target triple handling.
Recommended architecture: Option A (Python sidecar container per node). Recommended
algorithm: online SGD with EMA weight averaging and 50/50 replay buffer mixing.
