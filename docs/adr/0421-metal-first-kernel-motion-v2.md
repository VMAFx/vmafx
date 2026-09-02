<!-- markdownlint-disable MD013 MD060 -->
# ADR-0421: Metal first kernel — `integer_motion_v2` (T8-1c)

- **Status**: Accepted
- **Date**: 2026-05-11
- **Deciders**: lusoris, lawrence, Claude (Anthropic)
- **Tags**: `gpu`, `metal`, `apple-silicon`, `kernel`, `bit-exact`, `fork-local`

## Context

[ADR-0420](0420-metal-backend-runtime-t8-1b.md) (T8-1b) landed the Metal backend's runtime: real `MTLDevice` + `MTLCommandQueue` + `MTLBuffer` lifecycle, kernel-template helpers, accessor pair. Every public entry point works on Apple-Family-7+; the runtime returns `-ENODEV` elsewhere. Eight feature-extractor scaffolds under `core/src/feature/metal/` register their extractors against the runtime but each `init()` still returns `-ENOSYS` because no real Metal Shading Language kernel exists yet.

The strategic answer to "Mac GPU acceleration today" is the [Lusoris Homebrew tap](https://github.com/lusoris/homebrew-tap) shipping `enable_vulkan=enabled` (via MoltenVK). That works but adds a Vulkan → Metal translation hop. The endgame is native Metal kernels — bit-exact with the scalar reference per ADR-0214 — replacing the MoltenVK stopgap once a kernel proves the pipeline.

This ADR documents T8-1c: the first real Metal kernel. `integer_motion_v2` is the anchor because it's:

- The first kernel-template consumer (per ADR-0361 §"Scaffold + first consumer").
- A `VMAF_FEATURE_EXTRACTOR_TEMPORAL` consumer that exercises the cross-frame state plumbing (the hardest part of the consumer template).
- Already implemented on every other GPU backend (CUDA / SYCL / Vulkan / HIP), so the algorithm is fully understood and bit-exactness against the scalar reference is the contract the cross-backend-diff CI lane (ADR-0214) gates on.

The remaining seven feature-extractor scaffolds (`integer_motion`, `integer_psnr`, `float_psnr`, `float_motion`, `float_ssim`, `float_ansnr`, `float_moment`) follow as separate kernel commits within this same PR — mechanical replicas of the first-kernel pattern once the metallib pipeline + bridge ABI are proven.

## Decision

We will land a real `integer_motion_v2` Metal kernel and accept it as the load-bearing template every subsequent Metal kernel inherits.

### The kernel (`integer_motion_v2.metal`)

Two `kernel void` functions in [`core/src/feature/metal/integer_motion_v2.metal`](../../core/src/feature/metal/integer_motion_v2.metal):

- `motion_v2_kernel_8bpc` — `uchar` ref + cur samples; 32-bit y-conv accumulator; 64-bit x-conv accumulator (matches scalar reference's `int64_t accum` for the per-pixel x-conv path).
- `motion_v2_kernel_16bpc` — `ushort` samples via reinterpret + byte stride; 64-bit y-conv accumulator from the start (26386 × 65535 × 5 ≈ 8.6e9 overflows int32, same widening rule as the CUDA twin).

Both kernels:

- Use a 16 × 16 threadgroup with a 20 × 20 shared `int32` tile (radius-2 halo). Inner pitch padded to 21 to break threadgroup-memory bank conflicts (mirrors the CUDA twin's mitigation; Apple GPUs have a 32-bank threadgroup memory).
- Reduce per-thread `abs(h)` via MSL `simd_sum(uint)` to one value per 32-lane SIMD-group, then a single `atomic_fetch_add_explicit(&sad, lane_sum, memory_order_relaxed)` per group cuts atomic traffic 32×.
- Accumulate into a single `atomic_ulong` (64-bit; required on Apple-Family-7+; the runtime gate at `vmaf_metal_context_new` rejects pre-M1 devices, so the kernel never runs on hardware that lacks 64-bit atomics).
- Use edge-replicating reflective mirror padding (`2 * size - idx - 1` for `idx >= size`), matching the CUDA twin. Diverges from `motion_v1`'s `2 * size - idx - 2`; the CUDA file header documents the bring-up note.

### The dispatch (`integer_motion_v2_metal.mm`)

The T8-1 scaffold (`.c` returning `-ENOSYS`) is converted to Obj-C++ (`.mm`) and gains real dispatch logic:

- **`init`**: loads the embedded metallib via `dispatch_data_create` + `[device newLibraryWithData:]`, builds `MTLComputePipelineState` for both bpc variants, allocates a Shared-storage `MTLBuffer` for the `prev_ref` Y plane (unified-memory collapse of the HIP twin's `pix[2]` ping-pong).
- **`submit`**: on `index == 0` copies cur into prev and returns (no kernel — first frame has no "prev"). On `index > 0`: allocates a per-frame Shared-storage cur buffer, memcpys ref Y in, `blit fillBuffer` zeroes the SAD accumulator, `[encoder dispatchThreadgroups:]` with 16×16 tg + grid covering the frame, `[cmd commit]` + `waitUntilCompleted`, then copies cur into prev for the next frame.
- **`collect`**: reads the `atomic_ulong` SAD via `[buf contents]` (no D2H copy needed under unified memory), divides by `256.0 × W × H`, emits `VMAF_integer_feature_motion_v2_sad_score`. Emits `motion2_v2 = min(score[i], score[i+1])` once a previous score is available.
- **`flush`**: copies the last `motion_v2` score into the last `motion2_v2` slot (same as the CUDA / HIP twins).
- **`close`**: bridge-transfers all bridge-retained handles (pipeline states, prev_ref buffer) back to ARC, runs the kernel-template lifecycle close (drain → release queue → release events).

The dispatch uses the `vmaf_metal_context_{device,queue}_handle()` accessors added in T8-1b — no struct-layout coupling between `.mm` files.

### The metallib pipeline (`core/src/metal/meson.build`)

- `xcrun -sdk macosx metal -c <kernel>.metal -o <kernel>.air` (per-kernel custom_target)
- `xcrun -sdk macosx metallib <kernel>.air -o default.metallib` (single target combining every kernel into one library)
- Embedded into the libvmaf binary via the linker flag `-Wl,-sectcreate,__TEXT,__metallib,<path>` exposed through `declare_dependency(link_args: …)`.
- The consumer `.mm` reads the embedded byte range via linker-defined symbols `section$start$__TEXT$__metallib` / `section$end$__TEXT$__metallib`. No filesystem path dependency at runtime. Same embedded-blob pattern the CUDA backend uses for cubin.

### Smoke test

[`core/test/test_metal_smoke.c`](../../core/test/test_metal_smoke.c) stays at the T8-1b runtime-expectation shape (kernel-template lifecycle works, `state_init` returns 0 on Apple-Family-7+ or `-ENODEV` elsewhere). End-to-end bit-exactness against the scalar reference is checked by the `places=4` cross-backend-diff CI lane (ADR-0214) once an Apple Silicon runner is available.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Embed metallib via `-sectcreate` (chosen) | No filesystem path dependency at runtime; works for both shared- and static-library deployment; mirrors the CUDA embedded-cubin pattern | One `-Wl,-sectcreate` link arg + linker-defined symbols in the consumer | Smallest deployment friction; CUDA pattern is already understood in-tree |
| Ship `default.metallib` alongside `libvmaf.dylib` and load via `[device newDefaultLibrary]` | Standard Apple-app pattern; lets users hot-swap the metallib without relinking | Adds a deployment artifact; users have to ship two files; breaks the "single .dylib" expectation Homebrew has | Adds friction for every downstream consumer |
| Compile MSL from source at runtime via `[device newLibraryWithSource:]` | No build-time metallib; smallest source-tree footprint | Compile cost at every `init()`; pulls Apple's MSL compiler into the runtime via Metal framework (it's there but slow); reduces optimisation opportunities | Slowest path; the MSL source compile time can be hundreds of ms |
| One `.metal` file per kernel + one metallib per kernel | Per-kernel iteration cycles | Linker-arg surface multiplies; consumer .mm needs per-kernel section symbols | Single metallib covering every kernel is simpler |
| Use Vulkan-via-MoltenVK forever, skip native Metal | Zero new code | Pays MoltenVK translation forever; MoltenVK extension gaps already block one Vulkan feature path | Already shipping as the stopgap; the strategic answer is native Metal |

## Consequences

- **Positive**:
  - First working Metal feature kernel. `vmaf --backend metal --feature integer_motion_v2 …` actually runs on Apple Silicon and (once Apple Silicon CI validates bit-exactness) produces scores matching the scalar reference to `places=4`.
  - Establishes the consumer-side template every subsequent Metal kernel inherits: dispatch helper, embedded-metallib loader, bridge-retained pipeline state slots, per-frame Shared buffer pattern.
  - Unblocks T8-1d through T8-1k (7 remaining kernels). Each is a mechanical replica of T8-1c modulo the per-feature MSL body.
- **Negative**:
  - The first dispatch is synchronous (`[cmd waitUntilCompleted]` inside `submit_fex_metal`). Async submit/collect (events-based, mirrors the HIP twin's submit/finished pair) is a follow-up perf PR, not in scope here.
  - The cur Y buffer is allocated per-frame inside `submit`. Caching it on the state is a one-liner follow-up; correctness-first shape ships first.
  - 64-bit atomic gates on MSL 3.0+ / macOS 13+. The Apple-Family-7 runtime gate already restricts to M1+ (which is macOS 11+ in practice), so this is not an effective restriction beyond what's already enforced.
- **Neutral / follow-ups**:
  - **T8-1d–k** (the 7 follow-up kernels) — one commit each on this PR or a sibling PR. Add `.metal` + convert `.c` → `.mm` for each, append to the `mv2_air` pattern in `metal/meson.build`.
  - **Tap formula flip**: once Apple Silicon CI validates this kernel bit-exact at `places=4`, the [Lusoris Homebrew tap `libvmaf.rb`](https://github.com/lusoris/homebrew-tap/blob/master/Formula/libvmaf.rb) flips `enable_metal=enabled` (native) and demotes MoltenVK to a `--with-moltenvk` opt-in.
  - **FFmpeg patches**: a new `--enable-libvmaf-metal` configure flag in [`ffmpeg-patches/`](../../ffmpeg-patches/) gives FFmpeg's `-vf libvmaf=device=metal` switch parity with `device=cuda` / `device=vulkan`. Lands alongside the tap flip.
  - **Async submit/collect**: replace the inline `waitUntilCompleted` in `submit_fex_metal` with event-pair signalling (the kernel-template's `submit_pre_launch` + `collect_wait` helpers already implement this pattern; `submit_fex_metal` needs to wire its dispatch's command buffer to the events).

## References

- [ADR-0420](0420-metal-backend-runtime-t8-1b.md) — Metal backend runtime (T8-1b), this kernel's prerequisite
- [ADR-0361](0361-metal-compute-backend.md) — Metal backend scaffold (T8-1)
- [ADR-0192](0192-gpu-long-tail-batch-3.md) / [ADR-0193](0193-motion-v2-vulkan.md) — motion_v2 GPU port across backends
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4` bit-exactness gate (the validation contract)
- [ADR-0246](0246-cuda-kernel-template.md) — origin of the lifecycle template this Metal port replicates
- [`core/src/feature/cuda/integer_motion_v2/motion_v2_score.cu`](../../core/src/feature/cuda/integer_motion_v2/motion_v2_score.cu) — CUDA twin (algorithmic reference)
- Issue [#763](https://github.com/VMAFx/vmafx/issues/763) — T8-1b/c tracking
- [Lusoris Homebrew tap](https://github.com/lusoris/homebrew-tap) — currently ships MoltenVK stopgap; flips to native once this kernel validates
- Source: `req` — paraphrased: contributor asked for all eight Metal kernels in one PR; this is the anchor (T8-1c) with the remaining seven landing in the same PR as mechanical replicas.
