<!-- markdownlint-disable MD013 MD060 -->
# ADR-1336: Tear down CUDA resources in their owning context

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `cuda`, `correctness`, `lifecycle`, `testing`

## Context

The CUDA driver associates modules, streams, events, and device allocations
with the context in which they were created. In particular,
[`cuModuleUnload`](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__MODULE.html)
unloads a module from the current context. The feature-extractor close paths
instead called raw destroy functions without first making the extractor's
`VmafCudaState::ctx` current. A caller closing a VMAF context with no current
CUDA context, or with a different context current, could therefore fail to
release resources or operate on the wrong context.

The same audit found that `vmaf_cuda_kernel_lifecycle_init()` leaked the stream
or first event when a later event creation failed. Several module/function
lookup failures also left partially loaded modules alive. ADR-0982 recorded the
template leak as a follow-up; the complete inventory is now 19 feature owners
and 23 distinct module handles.

## Decision

Feature-owned CUDA modules, streams, and events will be destroyed through
shared internal helpers that push `VmafCudaState::ctx`, perform best-effort
teardown while preserving the first error, and restore the caller's previous
context. A handle is cleared only after the driver confirms destruction, so a
failed operation remains retryable. `VmafCudaKernelLifecycle` applies the same
rules during both partial-init rollback and close. Device-free fake-driver
tests exercise failure injection and foreign-context restoration, while a
source-inventory contract binds every module owner and rejects raw feature
teardown calls.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep raw teardown calls and require callers to make the owner context current | No implementation churn | The internal ownership contract leaks through every public caller and fails when close is invoked under a foreign context | Rejected: callers cannot safely infer every extractor's resource owner |
| Add an ad-hoc push/pop sequence to each extractor | Local changes only | Repeats error-prone cleanup in 19 files and permits future owners to drift | Rejected: the existing unload sweep already drifted across init and close paths |
| Centralize context-owned teardown and enforce a complete source inventory | One ownership rule, retryable handles, device-free failure coverage | Adds small push/pop overhead during init failure and close | **Chosen**: correctness and auditable ownership outweigh teardown-only overhead |

## Consequences

- **Positive**: Feature teardown no longer depends on an unrelated current
  context, and partially initialized lifecycle resources are reclaimed.
- **Positive**: Cleanup continues after individual failures, returns the first
  error, and preserves handles whose destroy operation failed.
- **Negative**: Closing extractors with several modules performs multiple
  context push/pop pairs; this is outside the scoring hot path.
- **Neutral / follow-ups**: Kernel arithmetic, scores, models, snapshots,
  public ABI, CLI behavior, FFmpeg patches, Netflix golden assertions,
  benchmarking, tuning, and training are unchanged.

## References

- [CUDA Driver API: Module Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__MODULE.html).
- [CUDA Driver API: Stream Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__STREAM.html).
- [CUDA Driver API: Event Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EVENT.html).
- [CUDA Driver API: Context Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__CTX.html).
- [ADR-0982](0982-gpu-runtime-bug-audit-round-26.md) — prior lifecycle audit and deferred template leak.
- [Research-2117](../research/2117-cuda-context-owned-resource-teardown.md) — inventory, failure model, and verification evidence.
- Source: `req` — “we fix everything until we cant find anything anymore for now”.
