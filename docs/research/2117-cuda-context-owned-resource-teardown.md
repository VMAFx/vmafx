<!-- markdownlint-disable MD013 -->
# Research-2117: CUDA context-owned resource teardown

- **Status**: Complete
- **Workstream**: [ADR-1336](../adr/1336-cuda-context-owned-resource-teardown.md)
- **Last updated**: 2026-09-26

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

The shared repair establishes five rules:

1. push the resource owner's context before module, stream, event, or raw
   device-buffer teardown;
2. quiesce and destroy streams before releasing objects they may reference;
3. clear a handle only after the corresponding driver release succeeds;
4. restore the previously current context, including the established one-retry
   unwind when the first pop reports failure; and
5. retain every owner container until all fallible child closes succeed.

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
retryable failed handles, phased stream quiescence, ring-picture retry, and
partial-init rollback. Vector, context-pool, worker-private, public-close, and
mixed-success ring-pool tests prove that a failed close never loses its owner.
The CUDA 13.4 build compiles every migrated feature translation unit. The two
device-free lifecycle tests pass, and the focused `float_ms_ssim_cuda` and
`psnr_hvs_cuda` parity tests pass on an NVIDIA GeForce RTX 4090 with driver
615.71.09 after the touched-file HISS refactors. No benchmark, tuning,
profiling, retraining, model, snapshot, dependency, public ABI shape, score,
or Netflix golden assertion changed. Public close semantics did change, and
the in-tree tools, embedded MCP compute handler, plus FFmpeg patch-stack callers
were adapted to retain the context on nonzero and invalidate it only on exact
zero.

## Open questions

- Hardware sanitizers do not report module backing-store leaks reliably, so a
  future real-device stress probe may add process-level driver-memory evidence.
- The helpers intentionally optimize for correctness at close, not push/pop
  minimization; batching several module unloads under one context guard would
  require a separate measured design.

## Related

- [ADR-1336](../adr/1336-cuda-context-owned-resource-teardown.md)
- `T-CUDA-CONTEXT-OWNED-TEARDOWN-2026-09-25` in `docs/state.md`

## Addendum: fail-closed teardown contract (2026-09-26)

The initial fix made individual handles retryable but left their enclosing
owners disposable. `context_destroy()` still freed `priv`; the context pool,
registered vector, and worker callback could still free pointer arrays after a
child close failed; and public `vmaf_close()` freed the top-level context after
ignoring those failures. The ring pool and drain stream had the same shape.

The follow-up separates teardown into fallible prepare and ownership commit:

- CUDA `close_required` is published before feature `init()` so a failed init
  remains visible without setting `is_initialized`.
- Context pools, vectors, and worker-private arrays retain all storage until
  every required close succeeds. Their destroy operations reject unprepared
  children with `-EBUSY`.
- `vmaf_close()` returns the first prepare error, normalized to negative errno,
  with the public context intact. A retry closes only remaining owners, then
  exact-zero success commits and invalidates the pointer.
- The ring pool records successful slots; retry never calls their callbacks
  again. A fully initialized CUDA picture commits stream, plane, and event
  releases individually. Drain-stream and primary-context failures retain the
  live state and driver table.
- An unimported `VmafCudaState` now releases its stream/context through
  retry-safe `vmaf_cuda_state_free()` rather than leaking them with a bare
  wrapper free. Import marks the caller wrapper so state-free remains
  allocation-only after context-owned teardown; duplicate imports fail closed.
- FFmpeg patch 0020 retains models and imported backend state until exact-zero
  close. Its dedicated CUDA filter also retains the source
  `AVHWFramesContext`, because the imported CUDA state borrows the device's
  `CUcontext` and a persistent close error outlives normal filter-link cleanup.
- `MotionForcezeroSourceContractTest` binds dict-before-force-zero ordering,
  close-callback preservation, and the device-free path in both CUDA motion
  files. The owner inventory additionally rejects legacy buffer-free helpers.

Scope is deliberately precise. Approximately 90 non-CUDA close callbacks have
not been audited for failed-init safety, so `close_required` is published only
for CUDA extractors; successfully initialized non-CUDA contexts still receive
normal close handling. The allocator-local `device_pic_unwind()` and
`device_pic_free_after_pop()` paths remain best-effort rollback of an object
that was never published to a ring pool. They are not covered by the retained
owner guarantee. Pool-construction rollback of earlier slots after a later slot
allocation fails has the same best-effort limitation because no retryable pool
owner is published. These paths should be a separate audit if retryable
half-built picture allocation becomes a requirement.

## Adjacent verification finding: Go link authority (2026-09-26)

Running the adapted Go callers against the branch-local libvmaf exposed the
deferred ADR-1125 defect: `pkg/libvmaf` embedded a
`-Lcore/build-cpu/src -lvmaf` directive. When that directory did not exist,
the linker kept searching and selected an installed stale library. The first
focused run then failed on missing `vmaf_dnn_*` symbols rather than exercising
this branch.

The binding now supplies headers only; every build authority supplies its
verified library explicitly. Local Make and Go CI select
`core/build-cpu/src`, while the server, controller, node, and dev-container
builders select the fork library staged into their image. This preserves both
in-tree and installed-container layouts without an implicit system fallback.

| Alternative | Consequence | Result |
| --- | --- | --- |
| Keep the embedded `-L... -lvmaf` | Convenient plain `go test`, but an absent directory silently searches the host | Rejected: original defect |
| Embed the exact in-tree `.so` path | Fails closed locally, but makes verified installed/container layouts impossible | Rejected |
| Require each build authority to set `CGO_LDFLAGS` | Explicit provenance at every caller; plain Go commands fail until configured | Chosen |

Verification covered both directions: focused Go tests and `go vet` pass when
pointed at `build-cuda-unwind/src`; the same package fails with unresolved
`vmaf_*` references when `CGO_LDFLAGS` is absent. The workflow contract test
pins Make, Go CI, and all four cgo container builders.
