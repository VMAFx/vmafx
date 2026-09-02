<!-- markdownlint-disable MD060 -->
# ADR-0338: macOS Vulkan-via-MoltenVK CI lane (advisory) for the Vulkan backend

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, vulkan, macos, moltenvk, gpu, advisory

## Context

The fork ships a working Vulkan compute backend
([ADR-0127](0127-vulkan-compute-backend.md),
[ADR-0175](0175-vulkan-backend-scaffold.md), and the kernel ADRs
0176–0252) with end-to-end coverage on Linux via Mesa lavapipe (CI)
and Intel anv (developer hardware). Apple Silicon is the canonical
hardware platform we have **no** automated coverage for: CUDA / SYCL /
HIP do not target macOS, and the planned native Metal backend is a
multi-month workstream tracked separately.

[MoltenVK](https://github.com/KhronosGroup/MoltenVK) is the
Khronos-supported open-source Vulkan-on-Metal translation layer.
If MoltenVK works against the fork's existing SPIR-V kernels, macOS
users get GPU-accelerated VMAF without waiting for the Metal port —
and the fork validates that SPIR-V → MSL translation is a usable
secondary route on Apple platforms.

The risk is real but bounded:

- Most of the fork's shaders use only non-atomic `int64` arithmetic
  (`GL_EXT_shader_explicit_arithmetic_types_int64`), which lowers to
  Metal's native `long` type and is well-supported on M1+.
- One shader (`moment.comp`) uses `atomicAdd` on `int64`
  (`GL_EXT_shader_atomic_int64`). Per the
  [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md),
  this requires Metal Tier-2 argument buffers — supported on M1+
  but the most fragile capability dependency in the shader set.
- Some Vulkan extensions used by the fork's runtime (notably
  `VK_KHR_external_memory_fd` for DMABUF import) are not supported
  by MoltenVK — but the smoke tests don't exercise the import path,
  and the host-staged copy fallback already exists (per ADR-0127
  §Decision).

CI cost on macOS Apple Silicon runners is non-trivial (`macos-latest`
billed at 10× the Linux rate per
[GitHub's billing schedule](https://docs.github.com/en/billing/managing-billing-for-your-products/managing-billing-for-github-actions/about-billing-for-github-actions#per-minute-rates)).
The lane is justified by the gap it closes — no other matrix entry
exercises any Apple-platform GPU code path.

## Decision

Add a single **advisory** CI lane to
[`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml):

- **Job name**: `Build — macOS Vulkan via MoltenVK (advisory)`
- **Runner**: `macos-latest` (Apple Silicon, Homebrew prefix
  `/opt/homebrew`).
- **Install**: `brew install -q molten-vk vulkan-loader vulkan-headers
  shaderc`.
  - Formula `molten-vk` lays `MoltenVK_icd.json` at
    `/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json` per the
    [Homebrew formula source](https://github.com/Homebrew/homebrew-core/blob/master/Formula/m/molten-vk.rb).
  - Formula `vulkan-loader` provides `libvulkan.dylib` (the loader
    volk dlopen()s).
  - Formula `vulkan-headers` provides the API headers consumed via
    the libvmaf wrap-fallback path.
  - Formula `shaderc` ships `glslc`.
- **ICD pin**: `VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`
  exported via `$GITHUB_ENV` so the loader is deterministic.
- **Build**: `meson setup … -Denable_vulkan=enabled` + `ninja`.
- **Smoke**: runs `test_vulkan_smoke`, `test_vulkan_pic_preallocation`,
  and `test_vulkan_async_pending_fence` against the live MoltenVK ICD
  and captures `vulkaninfo --summary` for triage.
- **Advisory mode**: `continue-on-error: ${{ matrix.experimental ==
  true && matrix.moltenvk == true }}`. Lane stays advisory until **one
  green run on `master`**, after which the
  `continue-on-error` reverts to default (`false`) and the job name
  is added to
  [`required-aggregator.yml`](../../.github/workflows/required-aggregator.yml).
- **Failure mode**: if a kernel pipeline fails to compile or
  enumerate on MoltenVK, the failing kernel + suspected MoltenVK gap
  is documented in
  [`docs/backends/vulkan/moltenvk.md`](../backends/vulkan/moltenvk.md)
  "Known limitations". Per
  [`feedback_no_test_weakening`](../../CLAUDE.md), thresholds are
  never lowered to make a failing kernel pass; the fix path is
  upstream MoltenVK or a kernel rewrite.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| MoltenVK CI lane (chosen) | Validates SPIR-V → MSL on real Apple Silicon hardware; reuses existing kernel set; closes the macOS GPU coverage gap today | One extra runner-minute cost; bounded by MoltenVK's translation gaps (atomicInt64, external memory) | Cheapest credible coverage of the fork's macOS GPU story |
| Wait for native Metal backend | Zero CI cost change | Leaves macOS without GPU coverage for the entire Metal port window (months); doesn't validate the SPIR-V translation path at all | Validation gap is real and the cost differential is small |
| MoltenVK on a self-hosted Apple Silicon runner | Stable hardware; no GHA macOS-runner billing | Requires a self-hosted runner registration we don't have today; secret-management overhead; inconsistent with the lavapipe lane shape | Premature optimisation — the GHA macOS runner is fine for advisory coverage |
| Required (not advisory) lane on day one | Forces every PR to confirm MoltenVK | If MoltenVK trips on a known-fragile capability (atomicInt64 / external memory), every PR red-lights regardless of whether the PR touches Vulkan | Advisory mode is the ADR-precedented pattern (cf. the Arc-A380 nightly lane in ADR-0127) |
| Build-only lane (no smoke test) | Cheaper; faster | Doesn't actually validate the shaders run on Metal — only that the C compiles | Defeats the purpose of the lane |

## Consequences

### Positive

- macOS is no longer a GPU-coverage hole. PRs that touch Vulkan
  runtime code surface MoltenVK regressions on the next push.
- The lane stress-tests SPIR-V → MSL translation on the fork's
  actual kernels, which feeds back into the native Metal backend
  decision: if MoltenVK works, the Metal backend's value is
  perf-only, not coverage-only.
- The operator-facing
  [`docs/backends/vulkan/moltenvk.md`](../backends/vulkan/moltenvk.md)
  gives macOS users a documented install + troubleshooting path
  with no Metal backend required.

### Negative

- One additional `macos-latest` job per PR — see GHA billing
  schedule for current cost. Mitigated by `if: github.event_name
  != 'pull_request' || github.event.pull_request.draft == false`
  (already shared with the rest of the matrix per ADR-0331), so
  draft PRs don't pay for it.
- MoltenVK is a moving target — its limitations matrix shifts with
  each release. The `moltenvk.md` known-limitations table is the
  canonical place to track current gaps; rebase notes call out
  that this table is hand-maintained.
- The lane's `continue-on-error` masks regressions until promoted
  to required. Promotion is a follow-up tracked in
  [docs/state.md](../state.md).

### Neutral

- No change to the existing Linux Vulkan lane, no change to any
  required CI check, no change to the Netflix golden gate.
- No public C-API surface change; the lane consumes existing
  smoke-test binaries.

## References

- [req] Implementation task brief 2026-05-09 paraphrased: add a macOS
  CI lane that builds + smoke-tests the existing Vulkan compute
  backend on macOS via MoltenVK, complementary to the native Metal
  backend dispatched separately.
- [ADR-0127](0127-vulkan-compute-backend.md) — Vulkan compute
  backend decision; this ADR appends a Status update to it.
- [ADR-0175](0175-vulkan-backend-scaffold.md) — backend scaffold
  the smoke tests pin.
- [ADR-0331](0331-skip-ci-on-draft-prs.md) — draft-PR skip pattern
  reused by this lane.
- [Research-0089](../research/0089-moltenvk-feasibility-on-fork-shaders.md)
  — feasibility digest: MoltenVK capability matrix vs the fork's
  shader inventory.
- [docs/backends/vulkan/moltenvk.md](../backends/vulkan/moltenvk.md)
  — operator-facing documentation.
- [Homebrew/homebrew-core `molten-vk.rb`](https://github.com/Homebrew/homebrew-core/blob/master/Formula/m/molten-vk.rb)
  — install layout, MoltenVK_icd.json path.
- [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
  — known limitations source for the moltenvk.md matrix.
- [CLAUDE.md §12 r10](../../CLAUDE.md) — doc-substance rule (this
  lane ships `docs/backends/vulkan/moltenvk.md` in the same PR).
- [CLAUDE.md §12 r11](../../CLAUDE.md) /
  [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverable
  rule.
