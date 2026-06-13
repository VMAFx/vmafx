---
name: add-gpu-backend
description: Scaffold a complete new GPU backend (hip, rocm, metal, opencl, etc.) with runtime, feature kernel stubs, public header, Meson options, CI workflow, doc stub, and smoke test. Flagship scaffolding skill. Established precedents on the fork — CUDA (legacy), SYCL (legacy), Vulkan (T5-1, ADR-0175 scaffold + ADR-0186 image-import).
---

<!-- markdownlint-disable MD013 MD033 MD060 -->

# /add-gpu-backend

Creates a fully-wired new GPU backend directory tree + build integration. Generated
code compiles as a stub (empty kernels returning zero scores) so the CI/build pipeline
stays green while the implementation is filled in incrementally.

The Vulkan (T5-1) scaffold is the canonical recent precedent — copy its structure
when adding a new backend. See
[ADR-0175](../../../docs/adr/0175-vulkan-backend-scaffold.md) for the scaffold
shape and [ADR-0186](../../../docs/adr/0186-vulkan-image-import-impl.md) for the
ffmpeg-patches contract that any new backend with image-import support inherits.

## Invocation

```text
/add-gpu-backend <name>
```

`<name>` is lowercase, e.g. `vulkan`, `hip`, `metal`, `opencl`, `rocm`.

## Files created

| Path                                                         | Purpose                              |
|--------------------------------------------------------------|--------------------------------------|
| `core/src/<name>/common.{c,h}`                            | Context/device selection             |
| `core/src/<name>/picture_<name>.{c,h}`                    | Picture allocation/lifecycle         |
| `core/src/<name>/<name>_types.h`                          | Backend-local types                  |
| `core/src/feature/<name>/integer_adm_<name>.{c,h}`        | ADM kernel stub                      |
| `core/src/feature/<name>/integer_vif_<name>.{c,h}`        | VIF kernel stub                      |
| `core/src/feature/<name>/integer_motion_<name>.{c,h}`     | Motion kernel stub                   |
| `core/include/libvmaf/libvmaf_<name>.h`                   | Public header (fetch pic, device sel)|
| `core/test/test_<name>_smoke.c`                           | Build + init + single-frame test     |
| `docs/<name>/README.md`                                      | Backend doc stub (build, caveats)    |
| `.github/workflows/<name>.yml` (or libvmaf-build-matrix.yml row) | CI job                           |

## Files patched

- `core/meson_options.txt` — new `enable_<name>` feature flag.
- `core/meson.build` — conditional subdir + dependency declaration.
- `core/src/meson.build` — feature sources + link.
- `core/src/libvmaf.c` — backend dispatch entry.
- `core/src/feature/feature_extractor.c` — registry add.
- `CLAUDE.md` + `AGENTS.md` — mention new backend in §1 "what this repo is".
- `docs/principles.md` — if the backend requires any rule relaxation, a PR-specific
  note (otherwise untouched).

## Templates

Located under `.claude/skills/add-gpu-backend/templates/` (not created by the skill —
authored by hand once for each candidate backend). Expected templates:

- `templates/vulkan/` — VkInstance, VkDevice, compute queue, SPIR-V shader skeleton
  (live; backs the T5-1 scaffold in `core/src/vulkan/`).
- `templates/hip/`    — hipInit, HIP kernel skeleton.
- `templates/metal/`  — MTLDevice, MTLCommandQueue, metal shader skeleton.
- `templates/opencl/` — clGetPlatformIDs, kernel skeleton.
- `templates/generic/` — fallback when no template exists: creates minimal no-op
  stubs + the CI job.

## Workflow

1. Validate `<name>` is a known backend (has a template) or fall back to generic.
2. Check no `core/src/<name>/` exists yet; if it does, refuse.
3. Copy template files, substituting `@NAME@`, `@NAME_UPPER@`, and `@COPYRIGHT@`
   (= `Copyright 2026 Lusoris`) placeholders.
4. Apply patches to shared files (search-and-replace-based).
5. Run `/build-vmaf --backend=<name>` to confirm the stub compiles.
6. Run `build/test/test_<name>_smoke` to confirm init path works.
7. Open a PR checklist comment with:
   - Backend implementation checklist (picture_<name>.c, feature kernels, sanitizer
     clean, ncu/vtune/rocprof profile).
   - CODEOWNERS review required.
   - Required CI checks listed.

## Guardrails

- Never overwrites existing files.
- Never activates the backend by default (`enable_<name>` starts as `disabled`).
- Never claims bit-exactness until `/cross-backend-diff` confirms ULP ≤ 2.
