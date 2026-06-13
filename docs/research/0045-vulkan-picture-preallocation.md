<!-- markdownlint-disable MD060 -->
# Research-0045: Vulkan picture preallocation — option-space digest

- **Date**: 2026-05-02
- **Companion ADR**: [ADR-0238](../adr/0238-vulkan-picture-preallocation.md)

## Question

CUDA + SYCL ship a public picture-preallocation surface (`*_preallocate_pictures`

- `*_picture_fetch`); Vulkan does not. What's the smallest surface
that closes the parity gap without locking the design to a single
host-allocator pattern?

## Reference surfaces compared

| Trait | CUDA (`libvmaf_cuda.h`) | SYCL (`libvmaf_sycl.h`) | Vulkan (this digest) |
|---|---|---|---|
| Methods | `NONE`, `HOST`, `HOST_PINNED`, `DEVICE` | `NONE`, `HOST`, `DEVICE` | `NONE`, `HOST`, `DEVICE` |
| Pool depth | Caller-controlled (via `vmaf_cuda_ring_buffer`) | Compile-time `pic_cnt = 2` | Compile-time `pic_cnt = 2` (mirrors SYCL) |
| Backing | `cudaMalloc` / `cudaMallocHost` / pinned | `sycl::malloc_device` / `sycl::malloc_host` | VMA `AUTO_PREFER_HOST` VkBuffer / regular host |
| Plane coverage | All planes | Y plane only | Y plane only (mirrors SYCL) |
| Buffer-type tag | `VMAF_PICTURE_BUFFER_TYPE_CUDA_*` | `VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE` | `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` (new) |
| Lock model | None (single-threaded fetch) | `std::mutex` | `pthread_mutex_t` (no C++ in vulkan/) |

The SYCL surface is the cleaner reference. CUDA's HOST_PINNED is a
CUDA-allocator-specific concept; VMA's `AUTO_PREFER_HOST` is the
closest analogue but isn't "pinned" in the CUDA sense — exposing it
under the same name would invite the wrong mental model.

## Decisions implied by the option survey

1. **Three methods, not four** — `NONE` / `HOST` / `DEVICE`. No
   `HOST_PINNED` analogue.
2. **Compile-time pool depth = 2** — matches SYCL exactly. Growing
   to a configurable depth is additive (mirror ADR-0251 follow-up
   #3's pattern of growing `VmafVulkanConfiguration` with an
   optional `unsigned`).
3. **Y-plane only DEVICE backing** — matches SYCL. Chroma kernel
   work in this fork still allocates its own buffers per-feature;
   no Vulkan extractor currently consumes preallocated chroma.
4. **`pthread_mutex_t` instead of C++** — `core/src/vulkan/` is
   pure C; the SYCL pool's `std::mutex` doesn't translate. The
   round-robin counter is the only contended state.
5. **Pool lives on `VmafContext`, not on `VmafVulkanState`** —
   matches both CUDA and SYCL. The state is the GPU resource
   handle; the pool is a per-context resource that's created via
   `vmaf_vulkan_preallocate_pictures` and torn down in
   `vmaf_close()`.

## Lifetime / fail-paths surveyed

The SYCL pool's allocation unwind on partial failure is the right
model: `pic_cnt` allocations attempted in sequence; on the i-th
failure, free pictures `[0, i)` and the pool struct itself, then
return the original error. The new C implementation
(`picture_vulkan_pool.c::pool_unwind`) preserves this contract.

The DEVICE method introduces a bookkeeping wrinkle:
`VmafVulkanBuffer` already maintains a host_ptr → buffer map for
the legacy `vmaf_vulkan_picture_alloc` shim. The pool path bypasses
that map by attaching the buffer handle to the picture's `priv`
cookie + `release_picture` callback — when the pool unrefs a pic,
the standard `VmafPicture` refcount path frees the buffer through
`VMA` directly. No double-free risk since the pool is the sole
owner of the cookie.

## Test contract

Six smoke tests pin the API contract without dispatching GPU work:

1. `NONE` is a no-op (returns 0, allocates nothing).
2. `HOST` allocates, fetches round-robin, unrefs cleanly.
3. `DEVICE` allocates, fetches, the host-mapped pointer is writable,
   ASan / UBSan don't flag a use-after-free on unref.
4. Fetch without preallocate falls back to a host-backed picture
   (mirrors the SYCL fallback contract for callers that ignore the
   preallocation surface).
5. Unknown method → `-EINVAL`.
6. NULL args → `-EINVAL` (no crash).

End-to-end scoring against pool-allocated pictures lives in the
cross-backend parity gate (unchanged — preallocation only changes
where bytes live, not which bytes the kernel reads).

## What this digest deliberately defers

- **Pool depth tunable**. SYCL has shipped at depth 2 for the
  full FFmpeg integration without complaint. If a real workload
  needs more, grow `VmafVulkanPictureConfiguration` additively.
- **Chroma plane preallocation**. Both SYCL and Vulkan kernels are
  luma-only today. A future U/V-aware extractor pulls this in.
- **External-handles parity**. The pool uses the imported state's
  VkInstance/VkDevice via the fork-internal
  `vmaf_vulkan_state_context()` accessor; external-handles callers
  (`state_init_external`) work transparently with no extra
  plumbing.
- **HOST_PINNED-like option**. VMA's allocator API has no "pinned
  in the CUDA sense" mode. If a workload turns up that needs it,
  the right path is a new method
  (`VMAF_VULKAN_PICTURE_PREALLOCATION_METHOD_HOST_VISIBLE_DEVICE_LOCAL`,
  ReBAR / SAM territory).

## References

- VMA `AUTO_PREFER_HOST` semantics:
  <https://gpuopen.com/learn/vulkan-renderers-memory-allocation/>.
- SYCL pool reference: `core/src/sycl/picture_sycl.cpp`
  (`vmaf_sycl_picture_pool_init`).
- CUDA preallocation reference:
  `core/include/libvmaf/libvmaf_cuda.h`,
  `core/src/libvmaf.c::vmaf_cuda_preallocate_pictures`.
- Parallel surfaces: ADR-0186 / ADR-0251 (zero-copy import) — not
  replaced by this PR; preallocation serves the host-driven path.
