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

Feature-owned CUDA modules, streams, events, and buffers will be destroyed
through shared internal helpers that push `VmafCudaState::ctx` and restore the
caller's previous context. A handle is cleared only after the driver confirms
its release. Teardown follows dependency phases: a stream must quiesce and be
destroyed before events or storage it may still reference are released. A
failed phase returns immediately with every still-live handle reachable for
retry.

CUDA extractor contexts publish a separate `close_required` state before
entering `init()`. It makes a failed partial initialization closeable without
misrepresenting it as successfully initialized. The registered vector,
extractor-context pool, and worker-private thread data each close their
contexts in a fallible prepare phase and free owner containers only in a later
commit phase. `vmaf_close()` runs those prepares before any commit; any nonzero
result retains a teardown-only `VmafContext` that must be closed again. Internal
positive pthread-style errno values are normalized to the public negative-errno
contract, but exact zero is the only ownership commit.

The CUDA drain stream, fully constructed ring pictures, ring-pool slots, and
`VmafCudaState` follow the same retained-owner rule. Device-free fake-driver
tests exercise failure injection and foreign-context restoration, while a
source inventory binds every module and buffer owner and rejects raw feature
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
- **Positive**: A failed phase returns its error without freeing the context,
  pool, vector, worker data, ring slot, or driver table that owns a live handle.
- **Negative**: Closing extractors with several modules performs multiple
  context push/pop pairs; this is outside the scoring hot path.
- **Negative**: A caller that receives any nonzero `vmaf_close()` result must
  retain imported backend state and model dependencies and retry close.
- **Neutral / follow-ups**: Kernel arithmetic, scores, models, snapshots,
  public ABI shape, Netflix golden assertions, benchmarking, tuning, and
  training are unchanged. Public teardown semantics changed; the in-tree tools,
  embedded MCP compute handler, and FFmpeg patch-stack callers are adapted to
  retain ownership on nonzero and invalidate it only on exact zero. The CLI
  reports persistent cleanup failure and exits non-zero.

## References

- [CUDA Driver API: Module Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__MODULE.html).
- [CUDA Driver API: Stream Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__STREAM.html).
- [CUDA Driver API: Event Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EVENT.html).
- [CUDA Driver API: Context Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__CTX.html).
- [ADR-0982](0982-gpu-runtime-bug-audit-round-26.md) — prior lifecycle audit and deferred template leak.
- [Research-2117](../research/2117-cuda-context-owned-resource-teardown.md) — inventory, failure model, and verification evidence.
- Source: `req` — “we fix everything until we cant find anything anymore for now”.

## Follow-up: fail-closed teardown contract (2026-09-26)

Independent review found that retaining an individual CUDA handle was not
enough when its owner container was still freed. The completed contract is:

1. **Partial init remains visible.** CUDA contexts set `close_required` before
   calling feature `init()`. `context_destroy()` returns `-EBUSY` until close
   succeeds. The flag is intentionally CUDA-only: roughly 90 non-CUDA close
   callbacks have not been audited for failed-init safety.
2. **Owner containers use prepare/commit.** Registered vectors, context pools,
   and worker-private contexts close all applicable children first. Any error
   retains the whole owner for retry. Destroy then becomes a commit-only phase;
   it never makes a surviving child unreachable.
3. **The public owner is retryable.** `vmaf_close()` drains workers, prepares
   worker-private contexts, pooled contexts, registered contexts, and the CUDA
   drain stream, then commits. Any nonzero return leaves the `VmafContext`
   teardown-only and owned by the caller. Tool binaries and the embedded MCP
   compute handler retry once and fail non-zero without freeing dependencies
   after a persistent error. FFmpeg patch 0020 applies the same rule; its
   dedicated CUDA filter additionally retains the `AVHWFramesContext` that owns
   the imported state's borrowed `CUcontext` until close succeeds.
4. **Backend teardown is phased.** GPU picture pools remember which slot
   callbacks committed and retry only failed slots. A complete CUDA picture
   destroys its stream before freeing planes and events, clearing each handle
   only after success. Drain-stream and `VmafCudaState` release failures retain
   their handles and function tables. An initialized CUDA state that was never
   imported releases those owners through retry-safe `vmaf_cuda_state_free()`;
   an imported wrapper remains allocation-only after exact-zero context close.
   Duplicate state imports and CUDA-owner overwrite attempts return `-EBUSY`.
5. **Device-free contracts bind the implementation.** Fake-driver tests cover
   retry after stream, event, plane, context-release, and pop failures. Static
   inventory covers all 19 CUDA feature owners and the buffer-owned helper
   migration; motion force-zero source order is checked separately.

Allocator-internal `device_pic_unwind()` and `device_pic_free_after_pop()`,
including pool-construction rollback of earlier slots after a later allocation
fails, remain best-effort because no retryable pool owner has been published.
This ADR's retained-owner guarantee covers CUDA extractor contexts and fully
constructed ring pictures; it does not claim retryability for those half-built
allocator-local objects.
