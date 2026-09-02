# ADR-0122: CUDA gencode coverage + actionable init-failure logging

- **Status**: Accepted
- **Date**: 2026-04-19
- **Deciders**: lusoris
- **Tags**: `cuda`, `build`, `docs`

## Context

Upstream Netflix `core/src/meson.build` ships CUDA cubins only at Txx
major-generation boundaries (sm_75, sm_80, sm_90, sm_100, sm_120) plus a
single PTX at the highest compute cap the host nvcc supports. On CUDA
12.x toolchains that PTX is `compute_90` or `compute_120`, neither of
which can JIT backward to Ampere `sm_86` (RTX 30xx) or Ada `sm_89`
(RTX 40xx). Those two architectures are the overwhelming majority of
consumer GPUs in the wild today; any user on a 3080/3090/4070/4090 who
builds libvmaf against a default CUDA 12.x toolchain ends up with a
library that has no runnable kernels for their GPU.

Separately, the CUDA init path in `core/src/cuda/common.c` returned
`-EINVAL` with only the message `"Error: failed to load CUDA
functions"` when `cuda_load_functions()` (the nv-codec-headers wrapper
around `dlopen("libcuda.so.1")`) failed. That is the single most common
first-time-setup failure mode on Linux, and the message offered no
diagnostic hint (no mention of the driver stub, the loader path, or
where to look). Not a regression — the message was already in upstream
— but the fork has first-class CUDA support as a selling point, so the
error UX matters.

A third, separate concern — the regression introduced by upstream
commit `32b115df` (experimental `VMAF_BATCH_THREADING` /
`VMAF_PICTURE_POOL` threading modes, +1255 lines including +227 to
`libvmaf.c` and the new `picture_pool.c` / `picture_pool.h`, 2026-04-07)
— is **out of scope for this ADR**. External reporter (lawrence,
2026-04-19) narrowed the runtime crash to the window in which the
fork rebased onto this commit; the lusoris tree began exhibiting the
same post-cubin-load crash that upstream Netflix/vmaf now does, even
with his downstream `vmaf-nvcc.patch` applied. This ADR covers only
the build-surface and init hardening; `32b115df` is tracked under
ADR-0123 (CUDA frame-submission regression vs 32b115df — experimental
threading modes) for focused bisect / revert / gate investigation.

## Decision

Two independent changes, shipped together because they share the PR /
CI cost:

1. **Extend `core/src/meson.build` to unconditionally include cubins
   for `sm_86` and `sm_89`** (in addition to the existing
   sm_75/sm_80/sm_90/sm_100/sm_120 entries), and **emit a `compute_80`
   PTX as an unconditional backward-JIT fallback** so every sm_80+ GPU
   that somehow lacks a matching cubin can still JIT a compatible
   kernel at driver load time. The old CUDA-version gating (only emit
   sm_86 if CUDA > 12.8, etc.) is removed; modern nvcc toolchains all
   support these archs.

2. **Harden `vmaf_cuda_state_init()` in
   `core/src/cuda/common.c`**: when `cuda_load_functions()` fails,
   log a multi-line actionable message that names the missing library
   (`libcuda.so.1`), the mechanism (dlopen via nv-codec-headers), the
   check command (`ldconfig -p | grep libcuda`), and the docs section
   (`docs/backends/cuda.md#runtime-requirements`). Fix a pre-existing
   memory leak on the error path by calling `cuda_free_functions()` +
   `free(c)` + zeroing `*cu_state` before returning. Similar treatment
   for the `cuInit(0)` failure path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Ship gencode fix only | Smaller diff | Leaves the actionable-error gap | Init logging change is a ~20-line edit and obviously useful; no reason to defer it |
| Ship NULL-guard fix only | Minimal risk | Does not address the gencode coverage hole | The gencode gap is a real shipping defect, not a cosmetic issue |
| Build a full fatbin covering every `sm_*` nvcc supports | Maximum GPU coverage | Binary-size cost, longer nvcc runtime at build | Diminishing returns past sm_86/sm_89 for the consumer target audience; compute_80 PTX already covers unusual sm_8x variants via JIT |
| Make gencode coverage user-configurable via meson option | Flexible | Another knob to document; most users won't know what to set | Sensible defaults beat knobs. Advanced users can still override via `-Dc_args` or a patch |

## Consequences

- **Positive**
  - Out-of-the-box libvmaf-cuda builds run on every currently-shipping
    consumer Nvidia generation from Turing through Blackwell, with no
    patches required.
  - First-time setup failures on Linux now produce a log line that
    tells the user exactly what is missing and how to check, instead
    of a terse "failed to load CUDA functions".
  - Pre-existing leak on the init error path is fixed.

- **Negative**
  - Two additional nvcc `-gencode` invocations at build time
    (sm_86, sm_89). Adds a small constant cost to every CUDA-enabled
    libvmaf build.
  - The static fatbin grows by the size of those two extra cubins per
    `.cu` source. Measurable but small against the libvmaf_cuda baseline.

- **Neutral / follow-ups**
  - `docs/backends/cuda/overview.md` gains a "Runtime requirements"
    section naming `libcuda.so.1` and the loader-path check command,
    matching the new log message.
  - Upstream commit `32b115df` (experimental `VMAF_BATCH_THREADING` /
    `VMAF_PICTURE_POOL` threading modes) is the lawrence-confirmed
    regression introducer for the post-cubin-load crash. Tracked
    under ADR-0123 for focused bisect / revert / gate investigation.
  - `CHANGELOG.md` gets a "lusoris fork" entry under the next
    release-please cut.
  - `docs/rebase-notes.md` gets an entry — the gencode change diverges
    from upstream meson.build's arch selection logic, so a future
    `/sync-upstream` will need to be aware.

## References

- Upstream `core/src/meson.build` gencode array (Netflix 2aab9ef1
  head).
- `ffnvcodec/dynlink_loader.h::cuda_load_functions` — the dlopen entry
  point libvmaf uses.
- External-user repro thread (2026-04-19, "lawrence"): user on
  Ampere (`sm_86`) observed upstream Netflix/vmaf crash until his
  downstream `vmaf-nvcc.patch` added sm_86 to the gencode array.
  Lusoris fork without the patch crashes identically. The patch also
  revealed a separate post-load crash that is **not** addressed here
  — see scope note in Context.
- Same thread (2026-04-19, later): lawrence identified upstream commit
  `32b115df92f04e715ad3efa1a66ae925dc69844d` (experimental
  `VMAF_BATCH_THREADING` / `VMAF_PICTURE_POOL` threading modes,
  2026-04-07) as the suspected regression introducer for the
  post-cubin-load crash — *"It wasn't until it rebased that the issues
  started happening in your fork too"*. Tracked under ADR-0123.
- `libvmaf.c:1447` `//^FIXME: move to picture callback` — predates
  `32b115df` but sits in the refactor perimeter; treated as a related
  hygiene item under ADR-0123.
- Source: `req` — user confirmed scope: *paraphrased:* "Ship gencode +
  NULL guard as defensive hardening, not as the root-cause fix; open a
  separate investigation into the post-cubin-load regression."
