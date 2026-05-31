<!-- markdownlint-disable MD013 MD060 -->
# ADR-0605: Renovate customManagers for all dev/Containerfile pinned dependencies

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `build`, `container`, `supply-chain`, `renovate`, `cuda`, `sycl`, `hip`, `intel`, `onnx`

## Context

`dev/Containerfile` is a multi-stage build that pins eleven distinct
versioned software components via `ARG` lines and hard-coded package names.
Before this ADR, only two groups were tracked automatically by Renovate:
(1) the base `ubuntu:` image (built-in Docker manager), and (2) the ROCm
apt-repo version (`ARG ROCM_VER`, added in ADR-0604). All other pinned
components were invisible to Renovate and would only be updated by a
manual audit.

The components with `ARG` pins are:

| ARG | Current value | Upstream |
|-----|--------------|----------|
| `NEO_VER` | `26.18.38308.1` | `github.com/intel/compute-runtime` |
| `GMMLIB_VER` | `22.10.0` | `github.com/intel/gmmlib` |
| `IGC_VER` | `2.34.4+21428` | `github.com/intel/intel-graphics-compiler` |
| `LEVEL_ZERO_VER` | `1.28.6` | `github.com/oneapi-src/level-zero` |
| `ORT_VERSION` | `1.26.0` | `github.com/microsoft/onnxruntime` |
| `SVTAV1_VERSION` | `2.1.0` | `gitlab.com/AOMediaCodec/SVT-AV1` |
| `VVENC_VERSION` | `1.14.0` | `github.com/fraunhoferhhi/vvenc` |
| `AMF_VERSION` | `1.5.2` | `github.com/GPUOpen-LibrariesAndSDKs/AMF` |
| `FFMPEG_TAG` | `n8.1.1` | `github.com/FFmpeg/FFmpeg` |

Two additional pinned items could not be tracked automatically (see
Alternatives considered):

- **`cuda-toolkit-13-2`** (apt package name): no `ARG` wraps the version;
  the major.minor is encoded in the package name as a dash-separated string
  (`13-2`) with no semver-compatible counterpart in any standard datasource.
- **`cuda-keyring_1.1-1`** (curl URL): the Debian-revision format (`1.1-1`)
  is not semver-compatible; `1.1` and `1` are indistinguishable under
  standard Renovate versioning.
- **`NV_CODEC_HEADERS_REF`**: a raw git commit SHA from
  `code.ffmpeg.org/FFmpeg/nv-codec-headers`; no release tags or versions are
  published for this mirror, making standard datasources inapplicable.

The `FFMPEG_TAG` ARG in the Containerfile was already partially covered: the
existing FFmpeg customManager matched the pin in the test scripts and CI gate
script, but not in the Containerfile itself. This ADR extends the FFmpeg
manager's `managerFilePatterns` to also scan the Containerfile.

## Decision

We will add nine `customManagers` entries to `renovate.json` covering every
`ARG`-pinned component in `dev/Containerfile`:

1. **FFmpeg** — extend the existing manager's `managerFilePatterns` to include
   `dev/Containerfile` and add a third `matchStrings` entry for
   `ARG FFMPEG_TAG=(?<currentValue>n[0-9.]+)`. Datasource: `github-tags`,
   depName `FFmpeg/FFmpeg` (unchanged).

2. **Intel compute-runtime (NEO)** — `ARG NEO_VER=(?<currentValue>[0-9.]+)`.
   Datasource: `github-releases`, depName `intel/compute-runtime`.

3. **Intel Graphics Compiler (IGC)** — `ARG IGC_VER=(?<currentValue>[0-9.]+)\+[0-9]+`.
   Captures the semver portion before the `+<build-id>` suffix.
   Datasource: `github-releases`, depName `intel/intel-graphics-compiler`.
   Known limitation: the `+<build-id>` suffix (Intel's internal build counter)
   is not updated by Renovate; a human reviewer must update it to match the
   new release's counter after Renovate bumps the semver part.

4. **Intel gmmlib** — `ARG GMMLIB_VER=(?<currentValue>[0-9.]+)`.
   Datasource: `github-releases`, depName `intel/gmmlib`.

5. **Level Zero loader** — `ARG LEVEL_ZERO_VER=(?<currentValue>[0-9.]+)`.
   Datasource: `github-releases`, depName `oneapi-src/level-zero`.

6. **ONNX Runtime** — `ARG ORT_VERSION=(?<currentValue>[0-9.]+)`.
   Datasource: `github-releases`, depName `microsoft/onnxruntime`.

7. **SVT-AV1** — `ARG SVTAV1_VERSION=(?<currentValue>[0-9.]+)`.
   Datasource: `gitlab-tags`, depName `AOMediaCodec/SVT-AV1`,
   `registryUrlTemplate: "https://gitlab.com"`.

8. **VVenC** — `ARG VVENC_VERSION=(?<currentValue>[0-9.]+)`.
   Datasource: `github-tags`, depName `fraunhoferhhi/vvenc`.

9. **AMF headers** — `ARG AMF_VERSION=(?<currentValue>[0-9.]+)`.
   Datasource: `github-tags`, depName `GPUOpen-LibrariesAndSDKs/AMF`.

All nine are grouped under a single `packageRules` entry with
`automerge: false` and `labels: ["dependencies", "dev-image"]`, since
container rebuilds have a large blast radius and require validation against
the host GPU stack.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Track CUDA toolkit via `nvidia/cuda` Docker Hub | Mature datasource, patch-level tracking possible | apt package name `cuda-toolkit-13-2` encodes version as `13-2` (dash-separated); Docker tags use `13.2.0-base-ubuntu24.04` — format mismatch prevents version comparison without unsupported transforms | Not chosen; requires adding `ARG CUDA_TOOLKIT_MAJOR_MINOR` to Containerfile to produce a comparable format |
| Track cuda-keyring via custom HTML datasource | Signals new keyring releases | Debian revision format `1.1-1` is not semver; the `-1` suffix is a packaging revision, not a patch version; Renovate's standard versioning would misparse it | Not chosen; deferred pending an `ARG CUDA_KEYRING_VERSION` refactor |
| Track NV codec headers via git SHA datasource | Would detect upstream commits | `code.ffmpeg.org/FFmpeg/nv-codec-headers` has no release tags; SHA-pinned deps require the `git` datasource with a manually configured ref, and Renovate cannot automatically determine when a newer SHA is "better" | Not chosen; too noisy and no clear version ordering |
| Use a single combined packageRule for all dev-image deps including ROCm | Simpler JSON | ROCm bumps have a distinct risk profile (KFD ioctl ABI) and were already grouped separately by ADR-0604 with a `manual-review` label; merging the rules would drop that label from ROCm | Not chosen; preserve ADR-0604's ROCm grouping |
| `automerge: true` for patch-only bumps | Less toil for minor updates | GPU stack components (NEO, IGC, Level Zero) have strong version coupling with the host kernel's i915/xe and KFD UAPI; a patch bump that shifts the Level Zero loader version can break the SYCL runtime even across minor versions | Not chosen; `automerge: false` for the full group |

## Consequences

- **Positive**: Eight previously invisible `ARG`-pinned components in the
  Containerfile now generate Renovate PRs when upstream tags new releases.
- **Positive**: The FFmpeg ARG in the Containerfile is now kept in sync with
  the test-script and CI-gate pins by the existing manager.
- **Negative**: The `+<build-id>` suffix in `ARG IGC_VER` will be stale after
  Renovate bumps the semver portion; reviewers must update it manually.
- **Negative**: Three components remain untracked (`cuda-toolkit`, `cuda-keyring`,
  `NV_CODEC_HEADERS_REF`) pending a Containerfile ARG refactor.
- **Neutral**: The ROCm `rocm/apt/` URL match string from ADR-0604 does not
  match the Containerfile because the URL uses `printf '%s'` with `${ROCM_VER}`
  (shell variable interpolation), not a literal version. The `ARG ROCM_VER=`
  match string is sufficient; the URL pattern is harmlessly non-matching.

## References

- `req`: user direction to extend Renovate with customManagers for every
  pinned dependency in `dev/Containerfile` (CUDA, NEO, IGC, AMF, SVT-AV1,
  VVenC, and others) so no dep goes untracked.
- [ADR-0604](0604-rocm-renovate-manager.md) — precedent: ROCm apt-repo customManager
- [ADR-0603](0603-ubuntu-26-04-fallout-fixes.md) — context for Intel NEO 26.18 + ROCm 7.2.3 + CUDA 13.2 pins
- [ADR-0569](0569-sdk-version-bumps-2026-05-18.md) — last manual SDK audit that bumped ORT, AMF, VVenC
- Renovate customManagers docs: `https://docs.renovatebot.com/configuration-options/#custommanagers`
- Renovate gitlab-tags datasource: `https://docs.renovatebot.com/datasources/#gitlab-tags`
