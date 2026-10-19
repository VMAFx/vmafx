<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3137: The one dev image carries the Vulkan loader and lavapipe, so Vulkan-dependent tests build and run on CPU in every lane

- **Status**: Accepted
- **Date**: 2026-10-10
- **Deciders**: lusoris (operator decision ci-config-19)
- **Tags**: ci, build, dev-container, vulkan, standards

## Context

The VMAFx Vulkan frame-import tests (RC4 WP3, ADR-2152, #2375) are built only where meson finds `dependency('vulkan')`. The dev image (`dev/Containerfile`) carries no Vulkan loader, so in that image, and in the hosted clang-tidy lanes that run in its pinned digest (`.github/actions/tidy-lane`), the tests are not built and the lanes cannot measure them: the cuda, hip and sycl lanes failed closed with "not measured" on five files #2375 recorded from a local image that had the loader. A Vulkan backend is not involved (ADR-0726 removed it); the loader is needed by tests of the import API.

## Decision

`dev/Containerfile` installs the Vulkan loader (`libvulkan1`), its headers (`libvulkan-dev`), Mesa's Vulkan drivers (`mesa-vulkan-drivers`, which carry lavapipe, the CPU Vulkan driver) and `vulkan-tools` in the `build-deps` stage, from the Ubuntu archive of the digest-pinned `DEV_BASE`, like the stage's other distribution packages. The build fails when `vulkaninfo --summary` does not list lavapipe. Every lane that builds in the image therefore builds the Vulkan tests, the tidy lanes measure them, and tests have a Vulkan device without a GPU. There is one image; real-GPU Vulkan runs stay a separate signal.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Loader and lavapipe in the one dev image (chosen) | One image, one digest; every lane builds the same tests; CPU Vulkan device everywhere | Image grows by the Mesa Vulkan drivers | Operator decision ci-config-19 |
| A second, Vulkan-enabled image for the GPU tidy lanes | Leaves the main image unchanged | Two images to pin, publish and keep in step; lanes diverge | Rejected by ci-config-19 (no second image) |
| Dated tidy-coverage exceptions for the Vulkan tests | No image change | The tests stay unmeasured in CI; an exception is a gap, not a fix | Rejected by Q-344 and ci-config-19 |
| Real-GPU runners for the Vulkan tests | Exercises real drivers | No hosted GPU runners; does not fix the tidy lanes | Kept as a separate signal |

## Consequences

- **Positive**: the Vulkan frame-import tests build and are measured in every lane of the one image; Vulkan code paths that need only a Vulkan device can run on lavapipe on any host.
- **Negative**: the image carries the Mesa Vulkan drivers; a pinned-image bump re-measures the tidy lanes' baselines.
- **Neutral / follow-ups**: the new image reaches the hosted lanes when the tidy-lane action's pinned digest moves to the published build of this change; #2375 moves it and re-measures. Tests that need a GPU's Vulkan memory (the import of a Vulkan image into CUDA, SYCL or HIP) keep choosing a GPU device and skip without one.

## References

- Operator decision ci-config-19 (relayed by praetor-07, ledger Q-345): add the Vulkan loader and the lavapipe CPU driver to the one pinned dev image; every lane builds and runs the Vulkan tests on CPU; no second image; real-GPU runs stay a separate signal; classed as a bug fix.
- Q-344: hold #2375 until it is decided how Vulkan-dependent tests are built in CI; no exceptions.
- ADR-2152 (the Vulkan frame import, #2375), [ADR-0726](0726-drop-vulkan-backend.md), [ADR-1102](1102-phase4b9-container-only-publishing.md), [ADR-0451](0451-local-dev-mcp-container.md).
