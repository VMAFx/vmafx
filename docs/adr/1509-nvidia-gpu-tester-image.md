<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1509: An NVIDIA GPU tester image that ships no NVIDIA file and measures every CUDA twin on a tester's GPU

- **Status**: Accepted
- **Date**: 2026-10-03
- **Deciders**: Lusoris
- **Tags**: ci, docker, cuda, testing, parity, license, fork-local

## Context

Every CUDA twin is declared exact (ADR-1457 and the fragments in
`scripts/ci/exact_twins.d/*.cuda`) on the strength of measurements on one GPU, the
RTX 4090 of a reference workstation (Ada, compute capability 8.9). The default build carries
cubins for `sm_80`, `sm_86`, `sm_89`, `sm_90`, `sm_100` and `sm_120` and PTX for
`compute_80` and `compute_120` (`core/src/meson.build`): five of those six cubins and
both PTX paths have never run. The maintainer asked for tester kits for hardware the
project lacks, on the condition that no licence is broken (ADR-1503); the Intel GPU kit
(ADR-1505) gave the report a backend-neutral `gpu` section for the CUDA and HIP kits to
fill.

Two facts shape the image. First, `libvmaf` links no NVIDIA library: it loads the
driver's `libcuda.so.1` at run time through the `nv-codec-headers` loader
(`core/src/cuda/common.c`), and its kernels are fatbins embedded in the library. In the
built image `readelf -d` of every binary names no NVIDIA library, so the image needs
none of the CUDA runtime files Attachment A of the CUDA Toolkit EULA would let it carry.
Second, the kernels' device code does contain NVIDIA code: nvcc inlines CUDA header code
and links functions of `libdevice.10.bc` into it. Attachment A lists `libdevice.10.bc`
and the floating-point and runtime-compilation headers as distributable; EULA 1.1.1(c)
allows them "as incorporated in object code format into a software application" that
meets 1.1.2 (material additional functionality, accessed only by the application, terms
consistent with the EULA).

ADR-1503 rule 7 allows a final stage `FROM nvidia/cuda:<v>-runtime` under the NVIDIA
Deep Learning Container License. The kit's brief prefers a plain base unless that layer
is needed.

## Decision

We will publish `ghcr.io/vmafx/vmafx:<describe>-tester-cuda` (linux/amd64) from target
`final-cuda` of `docker/Dockerfile.tester`, built and published by
`docker-publish-tester.yml`, whose Intel jobs become one matrix pair `build-gpu` /
`publish-gpu` over the kits (build, run without a GPU and require `no_device` naming
`--gpus all`, licence gate, push by digest, then in the `tester-publish` environment:
tag, keyless signature, provenance, a syft v1.51.1 SPDX SBOM attested with
`actions/attest`, and the `-source` image).

- **Build**: Debian 13 (`RELEASE_BUILDER_BASE`) with nvcc and cudart from NVIDIA's
  `debian13` repository at `build-config.env`'s exact versions
  (`scripts/ci/install-cuda-toolkit.sh --mode=builder`), `nv-codec-headers` at the
  commit `core/src/meson.build` requires, the default gencode list unchanged, the
  `gpu` suite's executables and the parity gate. The build records the CUDA version
  and targets from its Meson log in `image/cuda-targets.json`.
- **Shipped stage**: plain Debian 13 with its Python 3 and the VMAFx files, no NVIDIA
  file. `NVIDIA_DRIVER_CAPABILITIES=compute,utility` asks the NVIDIA Container Toolkit
  for the host driver's compute libraries; `LD_LIBRARY_PATH` includes
  `/usr/lib/wsl/lib` for WSL2. The build fails when a file named like an NVIDIA library
  appears in the image or a binary's `NEEDED` names one. Runs as uid 10001, read-only,
  `--network none`, `--cap-drop ALL`, with `--gpus all` (or the CDI device
  `nvidia.com/gpu=all`).
- **Licences**: artifact `cuda-image` of `licensing.json`. Component
  `nvidia-cuda-device-code` (no paths: the code is inside the VMAFx binaries) carries
  the CUDA Toolkit EULA, copied from the copyright file of the package that installs
  `libdevice.10.bc` (the build checks it is the v13.4 text of 2026-01-26), and notes that
  pass on 1.1.2 and 1.2 (accessed only by VMAFx, no reverse engineering, notices kept)
  and say that EUPL-1.2 covers only the VMAFx files. Component `nv-codec-headers` carries
  the two headers' MIT notices, copied from them at build time. `licensing.py` gains a
  `generated_build_files` rule form `compiled_from`: a kernel object
  (`src/<kernel>.fatbin.c`) takes the licence of the one `.cu` file it was compiled
  from. The gate runs in stage `cuda-licence-check`; the Debian sources go to
  `-tester-cuda-source`.
- **Report**: `hw_cuda.py` is the CUDA `GpuBackend`. It records how the container
  reached the GPU (`nvidia` device nodes, WSL2's `/dev/dxg` with
  `/usr/lib/wsl/lib/libcuda.so.1`, or `none` with the missing option), lists the
  devices through `hw_cudaprobe.py` (the driver API through ctypes in a bounded child
  process; name, compute capability, family, multiprocessors, memory, the driver's CUDA
  version; no UUID, no PCI bus ID) in PCI bus order, pins every run with
  `CUDA_VISIBLE_DEVICES=<n>` under `CUDA_DEVICE_ORDER=PCI_BUS_ID`, leaves out a device
  below compute capability 8.0 (ADR-1223) with the reason, and names the code path each
  device runs (the cubin the driver picks, or the PTX it compiles). CUDA has no audit
  test. The row map `cuda-rows.json` gives the new state row
  `T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03` a verdict per family (Ampere, Ada,
  Hopper, Blackwell): 13 named device tests and every parity-gate feature held exact on
  the four fixtures.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Final stage `FROM nvidia/cuda:13.x-runtime` (ADR-1503 rule 7) | NVIDIA's own runtime layer; the toolkit's documented pattern | Ships cudart and other files `libvmaf` never loads; adds the Deep Learning Container License's terms; NVIDIA publishes no image for every CUDA point release (the reason ADR-1306 left these images) | `readelf -d` shows the binaries need no NVIDIA library; a plain base ships nothing to license |
| Copy `libcudart.so` (Attachment A) next to `libvmaf` | Would let a later build link the runtime | Nothing links it today: an unused vendor file is what ADR-1503 rule 1 forbids | Not needed |
| Build on Ubuntu 26.04 (`CUDA_BUILDER`) as the production CUDA image does | Same base as the production image | Binaries need glibc 2.43, so the shipped stage would be Ubuntu too, with a second source-export path (Debian's snapshot fallback does not cover Ubuntu) | NVIDIA serves the same exact versions for `debian13`; the kit matches the CPU and Intel images |
| Separate `build-cuda` / `publish-cuda` jobs beside the Intel ones | No change to the merged Intel jobs | About 230 lines of workflow copied per kit; the HIP kit would copy them again (HISS-19) | One matrix pair, digests passed through an artifact |
| A per-family reduced gencode list | Smaller image | Every tester's card must find its code; ADR-1223 fixed exactly that gap | The default list ships, and the report names the path each GPU took |
| Run only the default device | Shorter run | A host with two different GPUs would measure one family | Every device (at most four), each pinned |

## Consequences

- **Positive**: one command measures every CUDA twin on any NVIDIA GPU of compute
  capability 8.0 or newer; the RTX 4090 run of the documented command passes in about
  2.5 minutes (66 device tests, 19 default-option features identical on four fixtures,
  98 gate cells at 0 and 2 skipped); the image carries no NVIDIA file, so the only
  NVIDIA terms are those of the code nvcc compiled into VMAFx, passed on in the notices.
- **Negative**: about 1.5 GB unpacked and 0.5 GB to download, mostly the device test
  executables, each linking `libvmaf` with its fatbins. The WSL2 path rests on Docker's
  and NVIDIA's documentation, not on a run of this image there. Reliance on 1.1.1(c)
  for the header code nvcc inlines is the reading every CUDA application relies on; the
  EULA does not spell it out for headers outside Attachment A.
- **Neutral / follow-ups**: the HIP kit joins the same matrix; a new CUDA version brings
  a new EULA, and the build's date check fails until the text and the record are read
  and updated.

## References

- `req` (maintainer, 2026-10-03, popup; paraphrased): prepare tester kits for the
  hardware the project lacks, Intel GPU, NVIDIA, AMD and later Windows, so outside
  testers can measure the twins; no licence may be broken.
- [ADR-1503](1503-tester-artifact-licensing.md), [ADR-1505](1505-intel-gpu-tester-image.md),
  [ADR-1223](1223-cuda-ampere-architecture-floor.md), [ADR-1306](1306-drop-nvidia-cuda-base.md),
  [ADR-1403](1403-cuda-strict-fp-every-kernel.md), [ADR-1457](1457-cuda-exact-twins-declared.md),
  [Research-2138](../research/2138-nvidia-gpu-tester-kit.md) (the measurements).
- [CUDA Toolkit EULA](https://docs.nvidia.com/cuda/eula/index.html), v13.4, last updated
  2026-01-26, read 2026-10-03: 1.1.1, 1.1.2, 1.2, 2.2, 2.3, Attachment A ("NVIDIA Common
  Device Math Functions Library: libdevice.10.bc", "CUDA Floating Point Type Headers",
  "CUDA Headers for Runtime Compilation", "NVIDIA CUDA Driver Libraries").
- [CUDA Toolkit 13.4 release notes](https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html),
  table 3: CUDA 13.x applications run on drivers 580 and later; new features of 13.4
  need R615. [NVIDIA Container Toolkit, specialized configurations](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/docker-specialized.html):
  `NVIDIA_DRIVER_CAPABILITIES` defaults to `utility,compute`.
  [Docker Desktop GPU support](https://docs.docker.com/desktop/features/gpu/) and the
  [CUDA on WSL user guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)
  (v13.4, 2026-09-13): `--gpus all` under Docker Desktop's WSL2 backend.
