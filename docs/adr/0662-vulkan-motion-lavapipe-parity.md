<!-- markdownlint-disable MD060 -->
# ADR-0662: Vulkan Motion Lavapipe Parity

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris maintainers
- **Tags**: vulkan, cuda, sycl, ci, feature-extractor, numerical-correctness

## Context

The lavapipe GPU-parity matrix had excluded `motion` and `motion_v2` because
`motion_vulkan` could crash inside the Mesa llvmpipe driver and
`motion_v2_vulkan` drifted from CPU by about `2.624e-3` on the Netflix normal
pair. That exclusion left a default-model feature outside the always-on software
Vulkan gate.

The audit found two concrete bugs. First, the stable `integer_motion_vulkan`
twin existed but was behind the legacy `motion_vulkan` registration and the CI
feature-name helper still routed `motion` to the legacy name. Second, the
`motion_v2` CUDA, SYCL, and Vulkan kernels used the stale high-edge mirror
literal from ADR-0193 (`2 * size - idx - 1`) while the CPU reference
`integer_motion_v2.c::mirror` uses reflect-101 (`2 * size - idx - 2`).

## Decision

Route Vulkan `motion` parity and model feature-name dispatch through
`integer_motion_vulkan`, keep the legacy `motion_vulkan` extractor available
only by explicit name, restore `integer_motion_vulkan`'s CPU/CUDA-compatible
`debug=true` default, and correct the CUDA, SYCL, and Vulkan `motion_v2` mirror
literal to `2 * size - idx - 2`. Re-enable `motion` and `motion_v2` in the
lavapipe matrix gate.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Raise tolerance or keep skipping `motion_v2` | Smallest CI change | Leaves a real CPU/GPU arithmetic bug and keeps the gate blind | Violates the fork's numerical-correctness posture |
| Fix only Vulkan `motion_v2` | Closes the observed lavapipe drift | Leaves CUDA and SYCL on the same stale mirror literal | The CPU contract is backend-agnostic |
| Delete the legacy `motion_vulkan` extractor | Removes the crash-prone name | Breaks explicit-name callers and historical docs | Compatibility can be preserved while routing automatic parity through the stable twin |
| Use `integer_motion_vulkan` for automatic dispatch and keep `motion_vulkan` explicit | Preserves compatibility and restores lavapipe coverage | Leaves a legacy explicit path that can still fail on llvmpipe | Chosen; direct legacy cleanup can happen in a later compatibility-deprecation PR |

## Consequences

- **Positive**: The always-on lavapipe matrix now gates `motion` and `motion_v2`
  again; `motion_v2` is exact against CPU on the fixture, and `motion` is within
  the existing `5e-5` gate.
- **Positive**: CUDA, SYCL, and Vulkan `motion_v2` kernels share the same
  high-edge mirror contract as CPU.
- **Negative**: `motion_vulkan` remains a compatibility name with the older
  two-buffer implementation; callers should use automatic backend dispatch or
  `integer_motion_vulkan` for lavapipe-stable parity.
- **Neutral / follow-ups**: A later PR may deprecate or internally alias the
  explicit `motion_vulkan` name once compatibility policy is settled.

## References

- Research digest: [0662 Vulkan motion lavapipe parity](../research/0662-vulkan-motion-lavapipe-parity.md).
- Prior ADR corrected here: [ADR-0193](0193-motion-v2-vulkan.md).
- Source: `req` ("well go on i guess we have enough backlog...").
