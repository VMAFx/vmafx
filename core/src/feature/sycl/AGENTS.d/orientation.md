---
paths:
  - core/src/meson.build
  - core/src/feature/sycl/sycl_compat.h
invariant: Orientation for agents on per-feature SYCL kernels and governing ADRs.
area: build-and-parity
---
<!-- markdownlint-disable MD013 MD060 -->
# Governing ADRs

- [ADR-0182](../../../../../docs/adr/0182-gpu-long-tail-batch-1.md) +
  [ADR-0188](../../../../../docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0192](../../../../../docs/adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batches. Every SYCL feature kernel here = row
  in one of these.
