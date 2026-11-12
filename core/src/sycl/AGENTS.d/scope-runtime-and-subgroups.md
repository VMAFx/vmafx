---
paths:
  - core/src/sycl/common.cpp
  - core/src/sycl/common.h
  - core/src/sycl/picture_sycl.cpp
  - core/src/sycl/dmabuf_import.cpp
  - core/src/feature/sycl/sycl_compat.h
invariant: SYCL runtime scope, sycl_compat.h macros, VmafSyclState aggregate init, duped dma-buf fds, and 16/32 subgroups.
---
<!-- markdownlint-disable MD013 -->
# SYCL scope, state init, dma-buf fds, and subgroup sizes

## Scope

SYCL-side runtime (queue management, USM, dmabuf import). SYCL
**feature kernels** live in [../feature/sycl/](../../feature/sycl/).

```text
sycl/
  common.cpp/.h             # queue creation, device selection, error-check
  picture_sycl.cpp/.h       # VmafPicture on a SYCL device (USM-backed)
  dmabuf_import.cpp/.h      # Linux DMA-BUF import path for zero-copy Level Zero
```

## Ground rules

- **Parent rules** apply in full: see [../../AGENTS.md](../../../AGENTS.md).
- **Compiler = `icpx` (Intel oneAPI) or AdaptiveCpp `acpp` / `syclcc`
  (ADR-0335).** `clang++ -fsycl` accepted in spirit, not CI-tested. Do
  not assume MSVC-style extensions.
- **Intel-specific kernel attributes go through
  `core/src/feature/sycl/sycl_compat.h`** (ADR-0335).
  `VMAF_SYCL_REQD_SG_SIZE(N)` macro expands to
  `[[sycl::reqd_sub_group_size(N)]]` under icpx, no-op under
  AdaptiveCpp. New kernel sites needing Intel-specific attribute -> add
  new macro to `sycl_compat.h`, do not hard-code attribute. **On
  rebase**: upstream cherry-pick bringing bare `[[intel::*]]` attribute
  on SYCL kernel lambda -> wrap in compat macro before merging.
- **Runtime invariants lint cleanup once broke (PR #1837 review).** Guard:
  `core/test/test_sycl_runtime_contract.py` (device-free).
  (1) `VmafSyclState` is aggregate, initialised in
  `vmaf_sycl_state_init()` as `new VmafSyclState{.queue = std::move(q),
  .copy_queue = std::move(cq)}`; `queue` and `copy_queue` stay its first two
  members. Never default-construct it and assign queues:
  default-constructed `sycl::queue` selects DEFAULT device and creates
  queue there (and context, when that device is on another platform).
  Measured: `--sycl_device 1` (OpenCL A380) then made 2 contexts + 5 queues
  over two devices; correct = 1 context + 3 queues on selected one
  (`SYCL_UR_TRACE=2`, count `urContextCreate` / `urQueueCreate`).
  constructor is not fix: it makes every member
  `misc-non-private-member-variables-in-classes` finding (that is why
  cleanup removed it).
  (2) A type C and C++ both read has ONE definition: no
  `#ifdef __cplusplus enum X : uint8_t` next to plain C enum (caller and
  callee disagree on size across `extern "C"`: `libvmaf.c` passes
  `enum VmafSyclPoolMethod` to `vmaf_sycl_picture_pool_init()`), no
  `using` / `typedef` pairs. Plain C form + cited NOLINT
  (`modernize-use-using`, `performance-enum-size`).
  (3) `vmaf_sycl_import_va_surface()` is `extern "C"`: every submit under
  it runs inside `dispatch_detile()`'s try; on throw
  `detile_submit_failed()` drains queue, frees import, returns
  `-EIO`. Level Zero descriptors of `vmaf_sycl_dmabuf_import()` are
  initialised with designated initialisers (valid `stype` from start,
  unnamed fields zero); `= {}` + assignments trips
  `bugprone-invalid-enum-default-initialization`, bare declarations leave
  future field uninitialised. Level Zero gets private duplicate of
  caller's dma-buf descriptor (`driver_fd()`, from high floor), closed
  afterwards unless driver closed it: compute runtime 26.35 closes
  descriptor of import that finds buffer already imported, and
  caller keeps its own (`libvmaf_sycl.h`). Never hand caller's descriptor
  to `zeMemAllocDevice()`; `test_sycl_dmabuf_fd_ownership` guards it.
- **Required sub-group size = 16 or 32, never 8
  ([ADR-1468](../../../../docs/adr/1468-sycl-sub-group-sizes-every-aot-target.md)).**
  Default build AOT-compiles every kernel for 19 targets
  (`sycl_icpx_aot_targets`); Xe2 (`lnl-m`, `bmg-g21`, `bmg-g31`) accepts 16
  and 32 only; one kernel requiring 8 fails its whole TU there ("Kernel
  compiled with required subgroup size 8, which is unsupported on this
  platform") = dev container image does not build (happened: #1703, one
  day). Lane builds (`-Dsycl_icpx_aot_targets=` or one device) never
  compile for Xe2 -> do not see it. Guards: `VmafSyclSubGroupSize<N>`
  static_assert in `sycl_compat.h` (any configuration);
  `core/test/test_sycl_sub_group_size_contract.py` (fast, device-free;
  also rejects raw `reqd_sub_group_size(` / `sub_group_size<` outside
  header); `meson test --suite sycl-aot`
  (`test_sycl_aot_default_targets.py`: every TU of build for full
  default list, ocloc, no device). Run suite before pushing kernel
  change from JIT-only or single-target build. New target in
  default list -> measure its sizes
  (`core/test/sycl_aot_targets.py::ocloc_accepts`) and add its family.
  Narrow shape wanted (row kernels: one work-item per row) -> 16.
- **Experimental flags enabled**: `-fsycl-unnamed-lambda`,
  `-fsycl-allow-func-ptr`, `-fsycl-device-code-split=per_kernel`. See
  [ADR-0027](../../../../docs/adr/0027-non-conservative-image-pins.md).
- **USM allocations**: prefer device USM for kernel-resident data,
  shared USM for cross-side communication. Host USM only when adjacent
  API requires pointer host can dereference directly.
- **dmabuf import** = Linux-only, gated at build time; no callers
  assume FD path exists on other OSes.
- **Numerical snapshots**: same rule as CUDA — see CLAUDE.md §9.
