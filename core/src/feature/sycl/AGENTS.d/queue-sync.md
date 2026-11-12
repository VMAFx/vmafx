---
paths:
  - core/src/feature/sycl/integer_*.cpp
  - core/src/feature/sycl/float_*.cpp
invariant: Per-step q.wait() in feature extractors forbidden — use in-order queues, barriers, or graph wait.
area: runtime-and-kernels
---
<!-- markdownlint-disable MD013 MD060 -->
# Device queue synchronization policy

- **Per-step `q.wait()` in feature extractors forbidden — use
  in-order queue** (ADR-0458 / SY-1). SYCL in-order queue serialises
  all submitted operations automatically; adding `q.wait()` between GPU
  kernels drains queue to idle, prevents pipelining. Only
  mandatory `q.wait()` calls at **CPU-reads-from-device boundaries**
  (i.e., right before host code reads `vmaf_sycl_malloc_host` buffer
  written by preceding `q.memcpy`). Example: `integer_cambi_sycl.cpp`
  has none of its own — `collect()` reads its readback after
  `vmaf_sycl_graph_wait()` (ADR-1357).
