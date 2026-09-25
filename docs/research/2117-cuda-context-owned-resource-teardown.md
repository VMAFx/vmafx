<!-- markdownlint-disable MD013 -->
# Research-2117: CUDA context-owned resource teardown

- **Status**: Complete
- **Workstream**: [ADR-1336](../adr/1336-cuda-context-owned-resource-teardown.md)
- **Last updated**: 2026-09-25

## Question

Can every CUDA feature extractor release its modules, streams, events, and
partial initialization state correctly when `vmaf_close()` runs with no CUDA
context current or with a different context current?

## Sources

- [CUDA Driver API: Module Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__MODULE.html)
  defines `cuModuleUnload` in terms of the current context.
- [CUDA Driver API: Stream Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__STREAM.html)
  documents stream ownership and `CUDA_ERROR_INVALID_CONTEXT` teardown failure.
- [CUDA Driver API: Event Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EVENT.html)
  documents event destruction and invalid-context failure.
- [CUDA Driver API: Context Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__CTX.html)
  defines the push/pop current-context stack used to preserve caller state.
- [ADR-0982](../adr/0982-gpu-runtime-bug-audit-round-26.md) identified the
  `VmafCudaKernelLifecycle` partial-handle leak as a deferred audit candidate.

## Findings

The source inventory contains 19 top-level CUDA feature translation units with
23 distinct module-handle fields. Before this repair, their unload calls used
whichever context happened to be current at close. The same raw-context
assumption existed for custom feature streams and events. A no-context or
foreign-context close could therefore return an invalid-context error while
leaving the resource live.

`vmaf_cuda_kernel_lifecycle_init()` created one stream and two events in order,
but its failure label deliberately retained successfully created earlier
handles. Function-lookup failures after `cuModuleLoadData()` had analogous
partial-module paths. The force-zero motion paths also allocated lifecycle
resources before selecting their CPU-like zero-emission behavior, making that
otherwise resource-free option depend on close cleanup.

The shared repair establishes four rules:

1. push the resource owner's context before module, stream, event, or raw
   device-buffer teardown;
2. attempt the complete cleanup set and return the first error;
3. clear a handle only after the corresponding driver destroy succeeds; and
4. restore the previously current context, including the established one-retry
   unwind when the first pop reports failure.

The motion force-zero decision now precedes CUDA resource creation. The SpEED,
ADM, PSNR-HVS, SSIMULACRA2, VIF, and template-based extractors use the same
context-owned teardown contract despite their different resource shapes.

## Alternatives explored

| Alternative | Evidence | Result |
| --- | --- | --- |
| Treat `CUmodule`, `CUstream`, and `CUevent` as globally self-identifying handles | NVIDIA documents current-context semantics and invalid-context errors | Rejected |
| Push/pop the owner context independently in each close callback | Existing raw teardown had already drifted across 19 owners and several custom lifecycle shapes | Rejected |
| Clear handles even when destruction fails | Prevents a later close or error unwind from retrying the live resource | Rejected |
| Shared owner-context helpers plus a complete owner inventory | Centralizes semantics and permits device-free fake-driver failure injection | Chosen |

## Verification evidence

The red-cap inventory initially found all 19 owners bypassing the helper. It
now requires the exact 19-file/23-handle set, requires every owner to call
`vmaf_cuda_module_unload()`, and rejects raw module, stream, or event teardown
throughout `core/src/feature/cuda/*.c`.

`test_cuda_runtime_unwind` uses a fake driver with a context stack and injected
push, pop, synchronize, destroy, creation, and unload failures. It proves owner
context selection, foreign-context restoration, first-error preservation,
retryable failed handles, complete best-effort close, and partial-init rollback.
The CUDA 13.4 build compiles every migrated feature translation unit. The two
device-free lifecycle tests pass, and the focused `float_ms_ssim_cuda` and
`psnr_hvs_cuda` parity tests pass on an NVIDIA GeForce RTX 4090 with driver
615.71.09 after the touched-file HISS refactors. No benchmark, tuning,
profiling, retraining, model, snapshot, dependency, public API, FFmpeg surface,
or Netflix golden assertion changed.

## Open questions

- Hardware sanitizers do not report module backing-store leaks reliably, so a
  future real-device stress probe may add process-level driver-memory evidence.
- The helpers intentionally optimize for correctness at close, not push/pop
  minimization; batching several module unloads under one context guard would
  require a separate measured design.

## Related

- [ADR-1336](../adr/1336-cuda-context-owned-resource-teardown.md)
- `T-CUDA-CONTEXT-OWNED-TEARDOWN-2026-09-25` in `docs/state.md`
