<!-- markdownlint-disable MD013 MD060 -->
# ADR-0738: Bump local CUDA toolkit pin to 13.3 + R610 minimum driver (partial — CI deferred)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `cuda`, `build`, `container`, `ci`, `deps`

## Context

CUDA 13.3 was released as GA on or before 2026-05-28. Confirmed facts:

- **Minimum host driver**: R610.43.02 (Linux x86_64).
- **New SM targets**: Blackwell `sm_100`, `sm_103`, `sm_120` added.
  `sm_103` is new in 13.3 (GB203 die — GeForce RTX 5070/5060 Ti class).
- **nvcc C++23**: CUDA 13.3 `nvcc` adds first-class C++23 support.
- **apt availability**: `cuda-toolkit-13-3_13.3.0-1_amd64.deb` confirmed present
  in the NVIDIA `ubuntu2404/x86_64` apt repository (verified 2026-05-28 via
  `developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/` listing).

Two distribution channels are **not yet updated** as of 2026-05-28:

- **Jimver/cuda-toolkit GitHub Action** (`src/links/linux-links.ts`): highest
  available version is `13.2.0`; `13.3.0` not yet present (verified).
- **NVCR `nvcr.io/nvidia/cuda`**: Docker Hub query returns `count: 0` for any
  tag containing `13.3`; no `13.3.x-devel-ubuntu24.04` tag published (verified).

The partial bump approach unblocks local development (dev container + host
builds with the R610 toolchain) while keeping CI stable on 13.2 until the
GitHub Action is updated.

## Decision

**Bump immediately (apt is available):**

1. `dev/Containerfile`: replace `cuda-toolkit-13-2` with `cuda-toolkit-13-3`.
   Update the comment block to reference R610.43.02 minimum driver and CUDA 13.3
   changelog items (sm_103, nvcc C++23). Supersedes ADR-0573.
2. `docs/backends/cuda/overview.md`: add `sm_103` row to the gencode table
   (gated `nvcc ≥ 13.3`); add minimum driver section with R610.43.02 note;
   add CUDA 13.3 toolkit notes section; reference this ADR.
3. `docs/development/build-flags.md`: update `enable_cuda` row to note
   R610.43.02 minimum with CUDA 13.3 toolkit.
4. `core/AGENTS.md`: add rebase-sensitive invariant for the CUDA 13.3 / R610
   contract so future rebases preserve the three-file update rule.

**Defer (not yet available):**

- CI `Jimver/cuda-toolkit` pin: stays on `cuda: '13.2.0'` in
  `.github/workflows/libvmaf-build-matrix.yml` until `13.3.0` appears in
  `Jimver/cuda-toolkit src/links/linux-links.ts`.
- NVCR `nvcr.io/nvidia/cuda` base image: stays on the 13.2 tag in any
  Dockerfile stages that use the NVCR image directly, until NVIDIA publishes
  a `13.3.x-devel-ubuntu24.04` tag.

## Alternatives considered

| Option | Verdict |
|---|---|
| Bump everything blind (apt + Jimver + NVCR) | Rejected — Jimver and NVCR do not yet carry 13.3. Changing the CI workflow pin to an unsupported version would break all CUDA CI legs immediately. |
| Wait for Jimver and NVCR before bumping anything | Rejected — the apt package is already available and local users (R610 driver, CUDA 13.3 toolkit installed) are blocked on the stale container. There is no reason to delay the apt / doc bump. |
| Partial bump (chosen) | Unblocks local development and documents the CI gap explicitly. CI follows when the action is updated; a future follow-up PR closes the gap. |
| Bump only docs, leave Containerfile | Under-delivers — the dev container is the primary build environment (CLAUDE §12 r15); leaving it on 13.2 while docs say 13.3 creates user confusion. |

## Consequences

- **Positive**:
  - Dev container and local host builds use the GA CUDA 13.3 toolkit.
  - `sm_103` gencode coverage for Blackwell RTX 5070/5060 Ti class GPUs.
  - Documentation accurately reflects the R610.43.02 minimum driver for 13.3.
  - No CI breakage — CI continues using the stable 13.2 Jimver pin.

- **Negative**:
  - Local container and CI are briefly on different CUDA toolkit versions (13.3
    vs 13.2). This is the same intentional split as the 13.1→13.2 transition
    (ADR-0573). The containers use the same upstream apt packages; no
    compatibility gap has been observed.

- **Neutral / follow-ups**:
  - When `13.3.0` appears in `Jimver/cuda-toolkit src/links/linux-links.ts`,
    open a follow-up PR to bump `.github/workflows/libvmaf-build-matrix.yml`
    and update this ADR's status note.
  - When NVCR publishes `13.3.x-devel-ubuntu24.04`, update any Dockerfile
    stages that reference the NVCR CUDA base image.
  - Consider enabling `--std c++23` in `core/src/meson.build` for CUDA TUs
    as a separate opt-in PR once the 13.3 toolkit is confirmed stable in CI.

## References

- req: "Bump what's actually bumpable NOW; defer what isn't. CUDA 13.3 GA
  released; apt repo confirmed; Jimver and NVCR not yet updated."
- NVIDIA apt repo listing (verified 2026-05-28):
  `developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/` —
  `cuda-toolkit-13-3_13.3.0-1_amd64.deb` present
- Jimver `src/links/linux-links.ts` (verified 2026-05-28): highest version
  `13.2.0`; `13.3.0` absent
- Docker Hub `nvidia/cuda` v2 API (verified 2026-05-28): `count: 0` for `13.3`
  name filter
- [ADR-0573](0573-dev-container-ubuntu-26-04-with-cuda-13-2.md) — superseded for
  the CUDA toolkit pin; ubuntu:26.04 base and other SDK decisions remain valid
- [ADR-0603](0603-ubuntu-26-04-fallout-fixes.md) — belt-and-suspenders
  `-D__MATH_NO_INLINES` nvcc flag retained; ADR-0603 §Consequences follow-up
  "When NVIDIA publishes `cuda-toolkit-13-3`: update the pin" is closed by this ADR
