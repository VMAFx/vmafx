### cambi_cuda: fix SIGSEGV on every input (Issue #857)

`cambi_cuda` segfaulted on every invocation regardless of resolution, bit
depth, or EOTF setting. The root cause was in the three kernel dispatch
helpers (`dispatch_mask`, `dispatch_decimate`, `dispatch_filter_mode`) in
`core/src/feature/cuda/integer_cambi_cuda.c`.

The CUDA driver API contract for `cuLaunchKernel` requires that each
`kernelParams[i]` points to the **value** to pass to the kernel — for a
device-pointer parameter this must be the address of a `CUdeviceptr`
variable, i.e. `&buf->data`. The three dispatch helpers were passing
`(void *)buf`, which is the host address of the `VmafCudaBuffer` struct.
The driver read `sizeof(CUdeviceptr)` bytes at that host address, which is
`VmafCudaBuffer::size` (a byte count, not a valid device address), and
handed it to the kernel as the image/mask pointer. The kernel dereferenced
this invalid device address on the first memory access, crashing the host
process with SIGSEGV.

Two secondary issues were also fixed: both the HtoD upload loop and the
per-scale DtoH readback loop performed pointer arithmetic by casting
`CUdeviceptr` (an unsigned integer) through `uint8_t *`; the arithmetic is
now performed directly on the `CUdeviceptr` type to eliminate undefined
behaviour.

All other CUDA extractors (`adm_cuda`, `vif_cuda`, `motion_cuda`, etc.)
were unaffected because they pass `VmafPicture` structs (by-value via the
kernel struct-copy semantics) rather than flat device buffers.

Fixes: Issue #857
