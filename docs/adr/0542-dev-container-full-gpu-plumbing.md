# ADR-0542: Full GPU backend plumbing in the dev-mcp container

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: `dev-container`, `cuda`, `vulkan`, `sycl`, `hip`, `rocm`

## Context

The `vmaf-dev-mcp` container (CLAUDE.md §12 r15) is the default
execution surface for every libvmaf / vmaf-tune / ai / MCP probe on
the dev machine. Its goal is reproducible cross-vendor GPU coverage:
NVIDIA via CUDA, Intel Arc via SYCL (Level Zero), Vulkan across all
three vendors, AMD via HIP. After ADR-0509 / ADR-0514 / ADR-0528
landed the bind-mounts and the level-zero ICD packages, the matrix
still produced **silent CPU fallback** or **software emulation** on
three of the four GPU lanes when empirically measured against a host
with NVIDIA RTX 4090 + Intel Arc A380 + AMD `gfx1036` (Raphael iGPU):

1. **Vulkan**: `vmaf --backend vulkan --vulkan_device 0` landed on
   `lvp_icd.json` (lavapipe, software). The mesa lavapipe ICD sorts
   lexicographically before `nvidia_icd.json` /
   `intel_icd.x86_64.json` / `radeon_icd.x86_64.json` and wins the
   `vulkan_device=0` index race when the loader enumerates the default
   ICD search path. The user-visible symptom was a Vulkan run that
   either returned lavapipe numbers (3–5× slower than CPU, no
   diagnostic) or segfaulted on shaders lavapipe does not implement.
2. **SYCL on Intel Arc**: `zeInit()` returned `0x70010000` and
   `sycl-ls` reported `Platforms: 0`. The level-zero loader was
   correctly installed but the VA-API user-space driver
   (`iHD_drv_video.so`) the Intel compute-runtime dlopens during GPU
   capability probing was missing.
3. **HIP on AMD `gfx1036`**: `rocminfo` returned "Unable to open
   /dev/kfd read-write: Invalid argument" and `hsa_init()` failed with
   `HSA_STATUS_ERROR_OUT_OF_RESOURCES`. The host's iGPU is **not** on
   the ROCm 6.x supported-GPU allowlist that `libhsa-runtime64.so`
   consults; without `HSA_OVERRIDE_GFX_VERSION` the runtime refuses to
   initialise even though `/dev/kfd` is bind-mounted and the `video` +
   `render` groups are joined.
4. **NVIDIA Vulkan ICD bind-mount**: depends on
   `NVIDIA_DRIVER_CAPABILITIES=compute,graphics,utility,video` (the
   `graphics` token is what causes the Container Toolkit to bind-mount
   `nvidia_icd.json` into `/etc/vulkan/icd.d/`). The compose file
   already carried that token, but no documentation pinned the
   invariant for future edits.

## Decision

We will close all four gaps in `dev/Containerfile`,
`dev/docker-compose.yml`, and `dev/scripts/dev-mcp-entrypoint.sh`:

1. Add `intel-media-va-driver-non-free` + `mesa-va-drivers` to the
   stage-1 apt list so VA-API codec drivers are present for both Intel
   and AMD.
2. Have the entrypoint dynamically rewrite `VK_DRIVER_FILES` to a
   colon-separated list of all non-lavapipe ICD JSONs visible under
   `/etc/vulkan/icd.d/` + `/usr/share/vulkan/icd.d/`. Lavapipe stays
   on disk as the CPU-only fallback but is filtered out whenever any
   real GPU ICD is present.
3. Pin `HSA_OVERRIDE_GFX_VERSION=10.3.0`, `HSA_ENABLE_SDMA=0`, and
   `ROCR_VISIBLE_DEVICES=0` in the docker-compose `common-env` block.
4. Document the `NVIDIA_DRIVER_CAPABILITIES` graphics-token invariant
   inline in `dev/docker-compose.yml` and in `dev/AGENTS.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Delete `/usr/share/vulkan/icd.d/lvp_icd.json` from the image | Most direct fix — loader cannot pick what is not on disk | Couples us to mesa's packaging (file moves between minor releases); breaks the CPU-only fallback path useful for hosts without GPU passthrough | Entrypoint-time `VK_DRIVER_FILES` rewrite gives the same outcome without coupling to mesa internals |
| Use `VK_LOADER_DRIVERS_DISABLE=lvp_icd` (Vulkan 1.3.250+ feature) | Single-line env var; documented Vulkan loader flag | Requires loader ≥ 1.3.250 baked into the image; Ubuntu 24.04 ships 1.3.275 so technically available but the disable token matches by filename glob in some loader versions and full-path in others — fragile across loader updates | `VK_DRIVER_FILES` allowlist semantics are stable across every loader release we ship |
| Switch to ROCm 7 to get `gfx1036` on the supported-GPU allowlist | No env-var override needed | ROCm 7 noble apt repo is not generally available yet (2026-05-18); switching means losing HIP coverage entirely until 7.x ships for noble | `HSA_OVERRIDE_GFX_VERSION=10.3.0` is the documented escape hatch and works on every ROCm 6.x release |
| Drop `mesa-vulkan-drivers` entirely (NVIDIA-only Vulkan) | Removes lavapipe by removing its package source | Loses Intel Arc + AMD Vulkan coverage — the whole point of cross-vendor parity | Cross-vendor coverage is a non-negotiable for the parity gate |

## Consequences

- **Positive**:
  - All five `--backend {cpu, cuda, sycl, vulkan, hip}` lanes return a
    real GPU score with no silent CPU/software fallback on the dev
    machine.
  - The `vmaf-tune compare` sweep can dispatch across every GPU
    backend in parallel without device-multiplex risk (CLAUDE.md §12
    r15 last bullet).
- **Negative**:
  - `HSA_OVERRIDE_GFX_VERSION=10.3.0` is a lie to the ROCm runtime;
    kernels that exercise gfx1030 features absent on `gfx1036` (e.g.
    wavefront-size-64 specific instructions) could in principle
    miscompile. Mitigation: the libvmaf HIP feature kernels stay on
    the ADR-0214 places=4 cross-backend gate; any regression surfaces
    in the regular CI matrix.
  - The entrypoint-time `VK_DRIVER_FILES` rewrite happens once at
    container start; new ICD JSONs installed via `docker exec apt
    install …` after startup will not be picked up until the next
    restart. Acceptable — package installs at runtime are an anti-
    pattern this container already discourages.
- **Neutral / follow-ups**:
  - When ROCm 7 ships in the noble apt repo, drop the
    `HSA_OVERRIDE_GFX_VERSION` override and re-evaluate whether
    `gfx1036` reaches the supported-GPU allowlist natively.
  - The lavapipe lane remains as the CPU-only fallback; a future ADR
    could add a `--vulkan-allow-software` opt-in flag in libvmaf for
    users who explicitly want software emulation (no current consumer).

## References

- [ADR-0509](0509-vulkan-icd-env-contract.md) — prior round that
  unset `VK_ICD_FILENAMES` / `VK_DRIVER_FILES` in the entrypoint.
- [ADR-0514](0514-dev-mcp-container-gpu-exposure.md) /
  [ADR-0528](0528-dev-dri-whole-directory-bind-mount.md) — GPU backend
  exposure invariants, `/dev/dri` whole-directory bind-mount.
- [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md) /
  [ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md) — HIP
  runtime works against `gfx1036` once initialised.
- [ADR-0540](0540-dev-mcp-ffmpeg-encoder-matrix.md) — FFmpeg encoder
  matrix companion change to the GPU matrix.
- ROCm supported-GPU list:
  <https://rocm.docs.amd.com/projects/install-on-linux/en/latest/reference/system-requirements.html>
- Vulkan loader env-var reference:
  <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md>
- Source: `req` — user reported the 4-backend silent-fallback matrix
  in the dispatch brief that opened this PR (paraphrased: "fix the
  remaining dev-mcp container GPU plumbing gaps so all four GPU
  backends actually run on real hardware, not lavapipe and not CPU
  fallback").
