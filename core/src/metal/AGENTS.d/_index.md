# AGENTS.md — core/src/metal

Orientation for agents working on Metal (Apple Silicon) backend.
Parent: [../../AGENTS.md](../../../AGENTS.md). Mirrors
[`core/src/hip/AGENTS.md`](../../hip/AGENTS.md) — HIP and Metal backends
share audit-first scaffold story. Metal's `MTLDevice` /
`MTLCommandQueue` / `MTLBuffer` API parallels HIP's `hipDevice_t` /
`hipStream_t` / `hipMalloc` surface closely enough that rebase /
refactor contracts on this side track HIP scaffold's.
