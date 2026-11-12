---
paths:
  - core/src/cuda/import_*.c
  - core/src/cuda/import_convert.cu
  - core/src/cuda/vmafx_cuda*.h
  - core/src/cuda/cuda_device_ptr.cuh
  - core/src/cuda/common.c
  - core/src/cuda/picture_cuda.c
invariant: VMAFx CUDA frame import/fences/pools, goto-free teardown helpers, VMAF_CUDA_DPTR macro, and .cu/.cuh lint rules.
---
<!-- markdownlint-disable MD013 -->
# VMAFx CUDA frames, teardown helpers, and device-code lint

## VMAFx device frames on CUDA (RC4 WP3, ADR-2023)

- Files: `vmafx_cuda.h` (what `core/src/vmafx/` calls), `vmafx_cuda_internal.h`,
  `import_device.c` (driver once per process, devices, engine attach),
  `import_frame.c` (checks, binding, conversions, picture attach),
  `import_fence.c` (release events, host release fences, CUDA_EVENT waits),
  `import_gl.c` (GL textures, GL sync), `import_pool.c`, `import_convert.cu`.
  They build into the library target, not `cuda_static_lib` (the CUDA common
  objects link into tests without the VMAFx sources).
- One library stream per device (`VmafxCudaDevice.state.str`); every frame of
  the device is a CUDA picture on it. Releases are enqueued there at the last
  reference: a host function signals HOST fences, the CUDA_EVENT release
  event is recorded. Every twin reads picture planes on the picture's stream:
  a twin that reads a picture on another stream must order that stream back,
  or a release can come before its read.
- Release events stay in the table of `import_fence.c` until recorded, so
  `vmafx_fence_wait()` answers pending; never record them at request time
  behind a stream waiting on a gate word (`cuStreamWaitValue32`): a blocked
  stream holds up every `cuCtxSynchronize()` and deadlocked two contexts.
- `VmafPicturePrivate.cuda.ordered`: the acquire fence was waited on by the
  library stream, so `libvmaf.c` skips the ADR-1199 barrier for the pair;
  pool frames and engine uploads stay unordered. `.cuda.vmafx`: never
  downloaded (`translate_picture_device()` refuses).
- Pool frames (`import_pool.c`): `VmafxFrame.lane_persistent` is an event
  recorded behind the frame's readers at release; `vmafx_cuda_pool_frame_arm()`
  waits on it before the frame goes out again (a frame returns to its pool
  before the device ran its readers). Pool frames are unordered: the
  ADR-1199 barrier orders the caller's writes (`test_pool_frames_ordered_by_barrier`).
- Bound planes: 8-byte aligned rows, pitch a multiple of 8 and at least the
  row rounded up (vif's vector loads); otherwise refuse naming offset / pitch,
  or a device copy with `VMAFX_IMPORT_ALLOW_COPY`. Conversion kernels read
  bytes and are bit-exact with the host upload; they mirror
  `core/src/metal/iosurface_layout.h`.
- nv-codec-headers lacks some `CUresult` values (`VMAFX_CU_ERROR_*`) and
  `cuDeviceTotalMem_v2` (resolved with `FFNV_SYM_FUNC`).
- Guards: `test_vmafx_import_cuda`, `test_vmafx_import_cuda_bitexact`,
  `test_vmafx_import_cuda_fence`, `test_vmafx_import_cuda_gl` (device, suite
  `gpu`/`cuda`), `test_vmafx_import_cuda_cells_contract.py` (fast).
- `test_vmafx_import_cuda_fence` skipped-wait control: late producer. From
  frame 1, distorted frame written only after its `CUDA_EVENT` release fence
  signalled (host-function gate, test opens it). No timed hold for that arm:
  adm readers outran 6 ms in ~1 run of 10. Frame 0 stays out: extractors'
  first-frame setup waits for producer stream.

## Teardown helpers replace the cleanup labels (HISS-21 / 2026-09-21)

`common.c` and `picture_cuda.c` have no explicit `goto` left. `provided_ctx_unwind` holds what
`fail` -> `fail_after_stream` fall-through used to do, and takes `ctx_pushed` so caller says
whether context pop still has to run; `pinned_alloc_unwind` takes `PINNED_UNWIND_*` stage
because it replaced `free_priv` -> `free_data` -> `fail_no_data` cascade. Adding resource to
either path means adding stage to helper, not second exit. `CHECK_CUDA_GOTO` labels stay
— they are macro's jump targets and now call helper.

## Device pointers and device-code lint (ADR-1142, RC3 tidy-zero)

- Kernel arg structs carry `CUdeviceptr` as `uint64_t`. Kernel turns one back into pointer with
  `VMAF_CUDA_DPTR(T, address)` (`cuda_device_ptr.cuh`), never `reinterpret_cast` on integer. One
  macro, one NOLINT(performance-no-int-to-ptr).
- Macro, not function: function wrapper (`reinterpret_cast` or `__builtin_bit_cast`) turned 134 of
  `speed_score.cu`'s 378 `LDG.E.CONSTANT` into plain loads (sm_89, nvcc 13.4). Do not turn it into
  an inline function.
- No designated initializers (`{.a = 1}`) in `.cu` / `.cuh`: nvcc's MSVC host frontend rejects them
  (`preflight.sh --stage msvcism`). Fill aggregate field by field (`make_*()` helper). No
  constructor: it trips `misc-non-private-member-variables-in-classes`.
- Helpers and device structs of a `.cu` live in `namespace {}`, no `static`. `__global__` kernels
  and symbols the host looks up by name stay `extern "C"` / external.
- Header included by C host (`typedef`, plain `enum`): `NOLINTBEGIN(modernize-use-using ...)`
  with ADR-1138 / ADR-1470 wording, like `core/include/libvmaf/model.h`.
- Refactor proof: sm_89 SASS before / after (`cuobjdump -sass`) identical, or the diff explained
  (commutative operand swap, register names).
- Loader member (`dev->state.f->cu*`) new to host code: present in nv-codec-headers
  `NV_CODEC_HEADERS_TAG` (`build-config.env`), else raise tag + commit + licence record in
  same PR and point `cc.has_member` check of `core/src/meson.build` at new member. Hosted CUDA
  legs clone default branch of headers: green there proves nothing for pinned images
  (`T-NV-CODEC-HEADERS-CI-LEGS-UNPINNED-2026-10-10`).
