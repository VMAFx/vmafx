<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — cmd/vmafx-controller

Go controller service, VMAFX distributed platform (ADR-0711, ADR-0709).
Exposes gRPC `VmafxController` (queue + node API) and `VmafxScoring`
(direct scoring) on single port, plus HTTP `/healthz /readyz /metrics /v1/score`.
