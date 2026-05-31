# ADR-0175: Vulkan compute backend — scaffold-only audit-first PR (T5-1)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, vulkan, scaffold, audit-first, fork-local

## Context

[ADR-0127](0127-vulkan-backend-decision.md) (Proposed) decided the
fork would add a Vulkan compute backend alongside the existing CUDA
and SYCL paths. The ADR sketched the runtime story (volk + glslc +
VMA + DMABUF; VIF as the pathfinder feature; lavapipe for CI smoke).
What it deliberately deferred: how to land that without a giant
single PR.

This ADR is the audit-first companion. Same shape as ADR-0173 for
the PTQ harness, ADR-0167 for doc-drift enforcement: ship the
**static surfaces** (header, build wiring, kernel stubs, smoke,
docs) in a focused PR so the runtime PRs that follow have a stable
base to land on.

## Decision

### Land scaffold only — no Vulkan SDK at build time

The PR creates:

- Public header
  [`core/include/libvmaf/libvmaf_vulkan.h`](../../core/include/libvmaf/libvmaf_vulkan.h):
  declares `VmafVulkanState`, `VmafVulkanConfiguration`,
  `vmaf_vulkan_state_init` / `_import_state` / `_state_free`,
  `vmaf_vulkan_list_devices`, `vmaf_vulkan_available`. Mirrors the
  CUDA + SYCL pattern.
- Backend tree under
  [`core/src/vulkan/`](../../core/src/vulkan/) — `common.{c,h}`,
  `picture_vulkan.{c,h}`, `meson.build`. All entry points return
  `-ENOSYS` or do-nothing.
- Kernel stubs at
  [`core/src/feature/vulkan/`](../../core/src/feature/vulkan/) —
  `adm_vulkan.c`, `vif_vulkan.c`, `motion_vulkan.c`. `_init` /
  `_run` entry points return `-ENOSYS`.
- New `enable_vulkan` feature option (default **disabled**) in
  [`core/meson_options.txt`](../../core/meson_options.txt).
- Conditional `subdir('vulkan')` in
  [`core/src/meson.build`](../../core/src/meson.build);
  `vulkan_sources` + `vulkan_deps` threaded through the `library()`
  call alongside the existing CUDA / SYCL / DNN aggregations.
- Smoke test
  [`core/test/test_vulkan_smoke.c`](../../core/test/test_vulkan_smoke.c)
  with 4 sub-tests pinning the scaffold contract
  (context_new / NULL-out / destroy-NULL / device_count returns 0).
  Wired in [`core/test/meson.build`](../../core/test/meson.build)
  under `if get_option('enable_vulkan').enabled()`.
- New CI matrix row "Build — Ubuntu Vulkan Scaffold (stub kernels)"
  in [`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml)
  that compiles with `-Denable_vulkan=enabled`.
- ffmpeg patch
  [`ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch`](../../ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch)
  — adds a `vulkan_device` option to the `libvmaf` filter, mirroring
  the SYCL selector in `0003-...patch`. Wires
  `vmaf_vulkan_state_init` / `_import_state` / `_state_free`
  alongside the existing CUDA / SYCL paths so the rebase story stays
  one patch ahead of the runtime.
- New docs at
  [`docs/backends/vulkan/overview.md`](../backends/vulkan/overview.md).

### Zero runtime dependencies for the scaffold

The scaffold has no `dependency('vulkan')`, no `volk`, no `glslc`,
no VMA. Adding those is the responsibility of the first kernel PR
per ADR-0127's "VIF as pathfinder" sequence. Reasoning: the
scaffold's CI run validates "the build wiring + meson dispatch +
test harness work end-to-end"; landing build deps before any kernel
uses them gates the scaffold's own CI green-light on a Vulkan SDK
that no code touches.

### Default `enable_vulkan` to `disabled`

The option is `feature` (auto / enabled / disabled), defaulting to
**disabled**. `auto` would silently flip on in builds that have a
Vulkan SDK installed; we want the scaffold to opt-in only until the
runtime PR is ready. Operators who want the scaffold's smoke test
to run flip `-Denable_vulkan=enabled` explicitly.

## Alternatives considered

1. **Land scaffold + runtime + first kernel in one PR.** Rejected:
   too large to review in one pass. Splits the trust boundary
   between "the scaffold compiles + smoke-tests" and "this kernel
   produces correct numbers" — different review skills, different
   CI gates. Audit-first separation per the same pattern as
   ADR-0173.
2. **Default `enable_vulkan` to `auto`.** Rejected for the
   silent-flip reason above. The scaffold isn't useful runtime —
   silently building stubs into a release-mode libvmaf would be
   confusing (`vmaf_vulkan_available()` returns 1 in that case but
   every other call is `-ENOSYS`).
3. **Skip the smoke test.** Rejected: 4 sub-tests cost ~5 ms total
   and pin the `-ENOSYS` contract. A future kernel PR that
   accidentally enables a code path (e.g. by linking against a real
   `dependency('vulkan')` without flipping the kernel-bodies) would
   trip the smoke test rather than landing silently broken.
4. **Skip the ffmpeg patch — only add it with the runtime PR.**
   Rejected: rebase risk. The fork's ffmpeg-patches series
   (`0001`..`0003`) needs to evolve in lockstep with the libvmaf
   API surface so a `/refresh-ffmpeg-patches` run against the next
   FFmpeg release reconciles cleanly. Adding `0004` now keeps the
   patch series in shape; the body is gated on
   `CONFIG_LIBVMAF_VULKAN` so the filter compiles unchanged when
   libvmaf is built without Vulkan.
5. **Use `vk-bootstrap` instead of writing the boilerplate.**
   Tracked for the runtime PR; out of scope here. ADR-0127 §
   "Library choices" pre-decided `volk` for symbol loading; the
   bootstrap question is for that follow-up.

## Consequences

**Positive:**

- Header surface lands without committing to runtime details.
  Future Vulkan-targeting consumers (third-party tools, MCP
  surfaces) can compile against the API today; calls fail
  cleanly with `-ENOSYS` until the runtime arrives.
- Build matrix gains a new lane that compiles the scaffold every
  PR — bit-rot is caught immediately.
- ffmpeg-patches series stays one patch ahead of the runtime,
  matching the pattern that worked for CUDA + SYCL.
- The `/add-gpu-backend` skill is exercised on a real backend
  for the first time; gaps in the template (missing
  `picture_vulkan.{c,h}`, wrong relative include path in feature
  stubs) surface during the audit-first PR rather than during a
  larger runtime PR.

**Negative:**

- Five new C files (1 public header, 5 implementation, 1 test)
  1 ADR + 1 doc + 1 ffmpeg patch with no functional code yet.
  Acceptable for an audit-first PR; the runtime PR will swap the
  bodies in place.
- `vmaf_vulkan_available()` returns `1` when built with
  `-Denable_vulkan=enabled` regardless of whether the kernels are
  real. A future PR can flip `vmaf_vulkan_available` to return
  `0` until the runtime PR lands, but that would break the
  scaffold's smoke test contract; for now the function honestly
  reports "the build was opted in", and operators read the docs
  for status.
- The ffmpeg patch declares `vmaf_vulkan_state_init` /
  `_import_state` / `_state_free` — so a fork-built ffmpeg with
  `--enable-libvmaf` and a libvmaf that has `enable_vulkan=enabled`
  will see a `vulkan_device` filter option that errors out on
  init. Documented in the patch commit message.

## Tests

- `core/test/test_vulkan_smoke.c` (4 sub-tests, all pass
  locally):
  - `test_context_new_returns_zeroed_struct`
  - `test_context_new_rejects_null_out`
  - `test_context_destroy_null_is_noop`
  - `test_device_count_scaffold_returns_zero`
- New CI lane: `Build — Ubuntu Vulkan Scaffold (stub kernels)` in
  the libvmaf build matrix. Compiles with
  `-Denable_vulkan=enabled` and runs the smoke test.

## What lands next (rough sequence per ADR-0127)

1. **Runtime PR**: `dependency('vulkan')` + `volk` + VkInstance /
   VkDevice / compute queue selection. `vmaf_vulkan_state_init`
   stops returning `-ENOSYS`. Smoke test contract shifts from
   "device_count returns 0" to "device_count >= 1 when a device
   is present, skip when none".
2. **VIF kernel PR**: first feature on the Vulkan compute path.
   Bit-exact-vs-CPU validation via `/cross-backend-diff`.
3. **ADM + motion kernels**: parity with the CPU + CUDA + SYCL
   matrix.
4. **lavapipe CI smoke**: Mesa software Vulkan, runs on a
   stock GitHub-hosted Ubuntu runner without a real GPU.
5. **`enable_vulkan` default flip** to `auto`: lets the build
   pick up Vulkan when present, only after the matrix proves
   bit-exactness.

## References

- [ADR-0127](0127-vulkan-backend-decision.md) — the Q2 governance
  decision this ADR implements (audit-first half).
- [ADR-0173](0173-ptq-int8-audit-impl.md) — the same audit-first
  pattern applied to the PTQ harness.
- [ADR-0167](0167-doc-drift-enforcement.md) — same two-layer
  audit pattern applied to docs / hooks.
- [`/add-gpu-backend`](../../.claude/skills/add-gpu-backend/SKILL.md)
  — the skill that produced the initial scaffold.
- [BACKLOG T5-1](../../.workingdir2/BACKLOG.md) — backlog row.
- `req` — user popup choice 2026-04-25: "T5-1 Vulkan backend
  scaffold (L, Recommended)".
