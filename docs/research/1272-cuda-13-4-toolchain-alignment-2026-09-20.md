<!-- markdownlint-disable MD013 MD060 -->
# Research-1272: CUDA 13.4.1 toolchain-alignment evidence

## Scope

This digest records the evidence used to repair Renovate PR #1487. It covers
current build and CI consumers only; historical CUDA 13.3 benchmarks, release
notes, changelog entries, and measured baselines remain historical evidence and
must not be rewritten.

## Artifact findings

| Consumer | Authoritative observation | Result |
|---|---|---|
| Container images | Docker Hub tag metadata for `nvidia/cuda:13.4.1-{devel,runtime}-ubuntu26.04` returned the digests already selected by Renovate | keep Renovate's digest pins |
| Dev container | NVIDIA `ubuntu2604/x86_64/Packages.gz` contains `cuda-toolkit-13-4` for 13.4.1 and later 13.4 updates | source `CUDA_APT_PACKAGE`; stop using the old ubuntu2404 workaround |
| Linux CI | the same repository contains `cuda-nvcc-13-4` and `cuda-cudart-dev-13-4` | install the compiler/runtime subset from NVIDIA apt |
| Jimver action | v0.2.36 `linux-links.ts` and `windows-links.ts` end at 13.3.1; no newer release or tag exists | remove the action from CUDA jobs |
| Windows x64 | HTTP range probe succeeds for `cuda_13.4.1_windows_x86_64_network.exe`; the pre-13.4 unsuffixed filename returns 404 | select the explicit x86_64 artifact |
| Windows ARM64 | HTTP range probe succeeds for `cuda_13.4.1_windows_arm64_network.exe` | enable the deferred native ARM64 compile |
| Windows packages | NVIDIA's 13.4 Windows guide lists `nvcc_13.4`, `cudart_13.4`, `crt_13.4`, `nvvm_13.4`, and `visual_studio_integration_13.4` | keep the minimal explicit package set |
| Architecture support | NVIDIA's 13.4 release table lists ARM64 Windows for NVCC, cudart, CRT, NVVM, and Visual Studio integration | build CUDA natively after the CPU ARM64 suite |

The official release notes assign CUDA 13.4 to driver branch R615. Existing
13.x applications retain minor-version compatibility at driver 580 or newer,
but 13.4 features and newly enabled platforms require R615; RTX Spark on
Windows requires 616.41 or newer. Documentation states both conditions rather
than collapsing them into one misleading minimum.

## Repository findings

- `build-config.env` already owns CUDA image tags but still declared 13.3.1
  and `cuda-toolkit-13-3`, so its own pins disagreed.
- `dev/Containerfile` copied `build-config.env` only after its literal CUDA
  install. Moving the copy before SDK installation lets both CUDA and Level
  Zero consume it.
- `core/src/meson.build` searched `HostX64/x64/cl.exe` before the compiler on
  `PATH`. On a native ARM64 runner that would feed the CUDA compiler an x64
  host compiler. The executable Meson regression now covers both PATH-first
  native selection and architecture-aware vswhere fallback.
- CUDA configuration requires `ffnvcodec/dynlink_cuda.h` and
  `dynlink_loader.h`; the ARM64 job therefore installs the same
  architecture-independent `nv-codec-headers` as the x64 jobs.
- The repository's actionlint v1.7.12 catalogue predates the
  `windows-11-vs2026-arm` hosted label. The exact label is registered alongside
  the existing Ubuntu 26.04 allowances so workflow lint remains strict without
  suppressing unknown-runner diagnostics.
- The CI-impact contract test found 19 Praetor, agent, and editor surfaces that
  master had never classified. They are now explicit global CI inputs; this
  repairs the contract without letting governance changes select fewer gates.

## Verification contract

```bash
python3 scripts/ci/check-workflow-versions.py
python3 -m unittest discover -s scripts/ci/tests -p 'test_*single_source.py'
python3 -m unittest core.test.test_windows_cuda_compiler_discovery
shellcheck scripts/ci/install-cuda-linux.sh
shfmt -d -i 4 -ci scripts/ci/install-cuda-linux.sh
scripts/ci/check-base-image-single-source.sh
```

The Windows installers and native ARM64 CUDA compilation require GitHub's
Windows runners and remain CI evidence, not a locally claimed pass. No live GPU
is present on those runners, so this PR does not claim Windows CUDA score
parity.

## Sources

- <https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/>
- <https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/>
- <https://developer.download.nvidia.com/compute/cuda/13.4.1/docs/sidebar/md5sum.txt>
- <https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64/Packages.gz>
- <https://github.com/Jimver/cuda-toolkit/tree/v0.2.36/src/links>
