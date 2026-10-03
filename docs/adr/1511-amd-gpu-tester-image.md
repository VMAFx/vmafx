<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1511: An AMD GPU tester image that ships only the ROCm runtime files the HIP build loads, with the source of its LGPL parts

- **Status**: Accepted
- **Date**: 2026-10-03
- **Deciders**: Lusoris
- **Tags**: ci, docker, hip, rocm, testing, parity, license, fork-local

## Context

Every HIP twin is declared exact (`scripts/ci/exact_twins.d/*.hip`) on the strength
of measurements on one GPU, the `gfx1036` graphics of the Ryzen 9 9950X3D in
`ryzen-4090-arc` (RDNA2, two compute units, wave32). CDNA GPUs run wave64, RDNA3 and
RDNA4 have other instruction sets, and none of their code objects has run. The
maintainer asked for tester kits for hardware the project lacks, on the condition that
no licence is broken (ADR-1503); the Intel and NVIDIA kits (ADR-1505, ADR-1509) fill
the report's backend-neutral `gpu` section.

Three facts shape the image. First, `libvmaf` links `libamdhip64`, whose ROCm 10.0.0
build links `libhsa-runtime64`, `libamd_comgr` (which links the bundled `libLLVM` and
`libclang-cpp`), `librocprofiler-register`, `librocm_kpack` and a set of system
libraries TheRock builds with renamed sonames (`rocm_sysdeps`: libelf, libnuma, libdrm,
libdrm_amdgpu, zlib, zstd, liblzma, libbz2), all found through RPATHs relative to
`libamdhip64`. Second, two of those are LGPL (elfutils' libelf, numactl's libnuma), so
their corresponding source must be published with the image; TheRock's manifest in the
image names the commit that built them, and TheRock pins the upstream archives by hash.
Third, ROCm under WSL2 needs AMD's WSL runtime and `librocdxg`, not the Linux HSA
runtime ROCm 10.0.0 ships.

The meson default list of offload targets in a build sandbox is
`gfx90a,gfx1030,gfx1036,gfx1100` (`core/src/meson.build`). HSA loads a code object only
for the exact gfx target, so an RX 6600 (`gfx1032`) or a 780M (`gfx1103`) would find no
code in such a build.

## Decision

We will publish `ghcr.io/vmafx/vmafx:<describe>-tester-hip` (linux/amd64) from target
`final-hip` of `docker/Dockerfile.tester`, as the third leg (`hip`) of the matrix jobs
`build-gpu` / `publish-gpu` of `docker-publish-tester.yml` (no-GPU run requiring
`no_device` naming `--device /dev/kfd`, licence gate, signature, provenance, attested
SPDX SBOM, `-source` image).

- **Build**: Debian 13 with `/opt/rocm` streamed out of the pinned ROCm image
  (`ROCM_BUILDER`) by `scripts/ci/install-rocm-from-image.sh`, which gains `--keep-docs`
  to keep the components' licence texts; `-Dhip_gfx_targets=` set to
  `HIP_GFX_TARGETS`, the targets ROCm 10.0.0 ships its own libraries for that testers are
  likely to own: `gfx908`, `gfx90a`, `gfx942`, `gfx950` (CDNA), `gfx1030` to `gfx1036`
  without `gfx1033` (RDNA2), `gfx1100` to `gfx1103`, `gfx1150`, `gfx1151` (RDNA3, 3.5),
  `gfx1200`, `gfx1201` (RDNA4). The build writes `image/hip-targets.json` (the targets
  from its Meson log, the ROCm release and TheRock commit from the image's manifest).
- **Shipped stage**: plain Debian 13 with its Python 3, the VMAFx files and only the ROCm
  files of `tools/rc1-tester/image/hip-runtime.json`, copied unmodified
  (`prepare_build.py rocm-runtime`, the generalised Intel runtime stager) into
  `/opt/vmafx/lib/rocm` with `llvm/lib` and `rocm_sysdeps/lib` beside them, so the RPATHs
  resolve without patching. Runs as uid 10001, read-only, `--network none`,
  `--cap-drop ALL`, with `--device /dev/kfd --device /dev/dri` and the groups of those
  nodes. Linux only.
- **Licences**: artifact `hip-image` of `licensing.json`: one component per ROCm part
  with the text ROCm installs (`share/doc/{hip,rocr,amd_comgr,rocprofiler-register}`) or
  a pinned `fetched_texts` entry (kpack and LLVM / Clang at the pins of the TheRock
  manifest, the bundled libraries at their upstream release tags, TheRock's licence).
  `licensing.py` gains `vendored_libraries` on a component: one rule per bundled library
  (licence, copyleft or not); a copyleft rule maps the ELF build IDs of the shipped file
  to source archives (elfutils 0.195 and numactl 2.0.19 as TheRock pins them, and the
  TheRock tree at the commit, with its patches and build scripts). The check fails on a
  rule that matches no file or on a copyleft library of another build; `sources` puts
  the archives into the `-tester-hip-source` image; positive and negative tests. Kernel
  objects (`src/<kernel>_hsaco.c`) take the licence of their `.hip` source
  (`compiled_from`).
- **Report**: `hw_hip.py` is the HIP `GpuBackend`. It records how the container reached
  the GPU (`kfd` with an AMD render node, or `none` with the missing option, WSL2's
  `/dev/dxg` named as unsupported), lists the GPU agents through `hw_hipprobe.py` (the HSA
  API through ctypes in a bounded child process: gfx target, family, product name,
  compute units, clock, PCI device ID; no UUID, no bus address), pins every run with
  `ROCR_VISIBLE_DEVICES=<n>`, and leaves out a GPU whose gfx target has no code object in
  the image, with the reason. HIP has no audit test. The row map `hip-rows.json` gives
  the new state row `T-HIP-TWINS-OTHER-TARGETS-2026-10-03` a verdict per family (CDNA,
  RDNA2, RDNA3 / 3.5, RDNA4): 14 named device tests and every parity-gate feature held
  exact on the four fixtures.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Final stage `FROM rocm/dev-ubuntu-26.04:10.0.0-full`, as `final-rocm10` does | Nothing to select | 29 GB with compilers, profilers, debuggers and math libraries; ROCgdb (GPL-3.0) and the compiler, which ADR-1503 forbids shipping | The report needs 0.25 GB of it |
| Copy the whole `rocm_sysdeps/lib`, as `node-rocm` does | One line | Ships GPL and LGPL libraries the runtime never loads (gmp, mpfr, ncurses, util-linux parts), each needing a source | Only the eight the closure needs ship |
| Point to TheRock and AMD's servers for the LGPL source | Nothing to publish | GPL-3.0 6(d) and LGPL-2.1 section 4 want the source at the same place (ADR-1503) | The source image carries the archives and the TheRock tree |
| Build inside the ROCm image (Ubuntu 26.04) and ship Ubuntu 26.04 | No cross-distribution copy | A second base and a second source-export path; pulling 29 GB does not fit a hosted runner | The streamed `/opt/rocm` on Debian 13 is what the CI HIP legs already use |
| The meson sandbox default `gfx90a,gfx1030,gfx1036,gfx1100` | Smaller image, faster build | Most RDNA2, RDNA3 and RDNA4 cards and every APU but one find no code object | Testers' cards are the point; the report names a GPU the list misses |
| Generic targets (`gfx10-3-generic`, `gfx11-generic`, `gfx12-generic`) | One object per family | Needs code object version 6, a flag core/src/meson.build does not pass; measures generic code, not what a native build of the tester's card runs | Native targets, listed |
| WSL2 support | Windows testers | Needs AMD's WSL HSA runtime and `librocdxg` instead of ROCm 10.0.0's; a second runtime set and licence record | Linux only; the report names WSL2 as unsupported |

## Consequences

- **Positive**: one command measures every HIP twin on any AMD GPU of the 17 listed
  targets on Linux; the gfx1036 run of the documented command is in Research-2139; the
  image ships no compiler, no profiler, no debugger, and publishes the source of its two
  LGPL libraries.
- **Negative**: the image is the largest of the kits (see Research-2139), mostly 17 code
  objects of every kernel in each of about a hundred test executables, plus 0.25 GB of
  ROCm runtime (`libLLVM` alone is 132 MB). A ROCm bump changes file names and the build
  IDs of the LGPL libraries, and the build fails until the record follows. The build
  downloads about 8 GB of the ROCm image on a cold cache.
- **Neutral / follow-ups**: WSL2 would need AMD's WSL runtime; `gfx1033`, `gfx1152`,
  `gfx1153` and `gfx1250` (which ROCm 10.0.0 also targets) can join the list when a
  tester asks.

## References

- `req` (maintainer, 2026-10-03, popup; paraphrased): prepare tester kits for the
  hardware the project lacks, Intel GPU, NVIDIA, AMD and later Windows, so outside
  testers can measure the twins; no licence may be broken.
- [ADR-1503](1503-tester-artifact-licensing.md), [ADR-1505](1505-intel-gpu-tester-image.md),
  [ADR-1509](1509-nvidia-gpu-tester-image.md), [ADR-1225](1225-rocm-10-therock-migration.md),
  [Research-2139](../research/2139-amd-gpu-tester-kit.md) (the measurements).
- ROCm 10.0.0 image `rocm/dev-ubuntu-26.04:10.0.0-full@sha256:8ebc02ee…` (`ROCM_BUILDER`):
  `share/therock/therock_manifest.json` (TheRock `16adc4d875fd4f65ea23c7c84e1c66706fde3047`,
  rocm-systems `6b0e43f341195e203754e08f850e437ff2fc09f9`, llvm-project
  `8f497e0992fb7513f7f78a6f6b6f1056c375e961`), `share/therock/dist_info.json` (the
  targets ROCm builds), `share/doc/*/LICENSE*`, read 2026-10-03.
- TheRock at that commit, `third-party/sysdeps/{linux/elfutils,linux/numactl,linux/libdrm,common/zlib,common/zstd,common/liblzma,common/bzip2}/CMakeLists.txt`
  (archives and hashes), read 2026-10-03; elfutils 0.195 (`libelf` headers:
  LGPL-3.0-or-later OR GPL-2.0-or-later), numactl 2.0.19 (`libnuma.c`: LGPL-2.1).
- [ROCm system requirements](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/reference/system-requirements.html)
  (2026-07-15); AMD's ROCm-on-WSL notes: the WSL runtime `hsa-runtime-rocr4wsl-amdgpu`
  and `librocdxg` replace the Linux HSA runtime under WSL2.
