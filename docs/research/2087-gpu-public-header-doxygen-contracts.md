# Research-2087: GPU public-header Doxygen contracts survived in code, not prose

## Finding

Commit `f712f8e6d` (PR #712) documented CUDA state ownership and the SYCL
picture-preallocation enum. Its direct successor `df170516c` removed those
comment-only hunks while doing unrelated CI cleanup. Later work restored some
thread-safety notes and the detailed CUDA preallocation enum, but four
load-bearing public contracts were still absent at `4e6916d16`:

- `vmaf_cuda_state_init()` returns a caller-owned allocation through an output
  parameter;
- CUDA import copies that state by value without transferring ownership, so
  `vmaf_close()` precedes `vmaf_cuda_state_free()`;
- CUDA state-free takes a single pointer and cannot NULL the caller's handle;
- the SYCL preallocation methods have stable values and distinct allocation
  paths.

The historical import prose said the context "borrows the state pointer".
That is not what current code does: `core/src/libvmaf.c` assigns
`vmaf->cuda.state = *cu_state`. The restoration therefore preserves the useful
ownership intent but documents the live by-value copy instead of replaying the
stale sentence.

## Source-of-truth audit

| Contract | Current implementation evidence |
| --- | --- |
| CUDA init output ownership | `core/src/cuda/common.c` allocates through `*cu_state`, clears it on either post-allocation failure, and returns the live allocation only on success. |
| CUDA import ownership | `core/src/libvmaf.c` copies `*cu_state` into `vmaf->cuda.state`; `core/src/cuda/common.c` documents and implements the caller's later `free()` of the original allocation. |
| CUDA free shape | The public signature is `vmaf_cuda_state_free(VmafCudaState *)`, unlike the double-pointer SYCL/HIP/Metal family; it cannot update the caller's variable. |
| SYCL `NONE=0` | `vmaf_sycl_preallocate_pictures()` creates no pool; `vmaf_sycl_picture_fetch()` falls back to `vmaf_picture_alloc()`. |
| SYCL `DEVICE=1` | The switch selects `VMAF_SYCL_POOL_DEVICE`, whose allocator calls `sycl::malloc_device`. |
| SYCL `HOST=2` | The switch selects `VMAF_SYCL_POOL_HOST`, whose allocator calls `sycl::malloc_host`. |

Making `DEVICE` and `HOST` explicit does not change ABI values: C already
assigned the implicit successors 1 and 2. It makes the append-only public
contract reviewable and prevents an inserted enumerator from silently
renumbering downstream FFI values.

## Alternatives considered

| Alternative | Result |
| --- | --- |
| Cherry-pick `f712f8e6d` | Rejected. It also edits the removed Vulkan surface and carries the inaccurate borrowed-pointer sentence. |
| Restore comments without a test | Rejected. That is exactly the shape silently removed by `df170516c`; Doxygen warning-clean output cannot detect missing semantics. |
| Selectively restore current semantics and add an executable source contract | Chosen. It preserves later header work and tests meaning rather than whitespace. |

No ADR is needed: this restores documentation of existing behavior and makes
two already-effective enum values explicit; it makes no architectural,
policy, ABI, or runtime decision.

## Red-cap and verification

`python3 -B core/test/test_gpu_public_header_docs.py` failed on the untouched
base with 15 missing-contract assertions across five tests. After the
restoration, the same five tests pass. The test extracts the Doxygen block
attached to each declaration, normalizes formatting, and checks semantic
markers, so reflowing prose does not cause a false failure.

The public Doxygen build reproduced the committed warning ratchet at 228 of
228 lines, the Meson-registered focused test passed 1 of 1, and `make
verify-all` passed with all 11 touched files HISS-clean, 18 of 18
HISS claims replayed, and a 100% deduplication score.
