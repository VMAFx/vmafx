<!-- markdownlint-disable MD013 MD024 MD046 -->
# Test VMAFx on your hardware without building anything

This page is for someone who has a machine the project does not own (an Apple
M-series Mac, an Intel GPU, an Ampere or Graviton box, an unusual x86 CPU) and wants
to check the fork on it. You build nothing, install no toolchain and need no
repository checkout. You run one prepared package, it prints one JSON report, and you
can send that report to the project and be credited for it.

There are five packages. On a Mac, run the **native bundle** first: it also exercises
the Metal backend, which no container can reach. The **container image** tests the
CPU code paths and works on any machine with Docker. The **Intel GPU image** tests the
SYCL backend on an Intel GPU (integrated UHD or Iris Xe graphics, Arc, Data Center),
on Linux or on Windows with WSL2. The **NVIDIA GPU image** tests the CUDA backend on
an NVIDIA GPU (GeForce RTX 30 series or newer, RTX professional cards, A100, H100,
B200). The **AMD GPU image** tests the HIP backend on an AMD GPU (Radeon RX 6000, 7000
and 9000 series, Ryzen graphics, Instinct), on Linux.

| | Native macOS bundle | Container image | Intel GPU image | NVIDIA GPU image | AMD GPU image |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Runs on | macOS on Apple silicon | any Docker host (Linux arm64 or amd64, Docker Desktop) | Linux x86-64 with an Intel GPU, or Windows 11 with WSL2 | Linux x86-64 with an NVIDIA GPU, or Windows with WSL2 (not yet proven) | Linux x86-64 with an AMD GPU (not Windows) |
| Exercises | NEON default dispatch against scalar, **every Metal twin against the CPU**, SIMD unit tests | NEON (or AVX2 / AVX-512) default dispatch against scalar and against baked references, SIMD unit tests, the Netflix golden gate | AVX2 / AVX-512 default dispatch against scalar, **every SYCL twin against the CPU on every Intel GPU**, the parity gate, the SYCL device tests and the scratch-memory audit | AVX2 / AVX-512 default dispatch against scalar, **every CUDA twin against the CPU on every NVIDIA GPU**, the parity gate, the CUDA device tests | AVX2 / AVX-512 default dispatch against scalar, **every HIP twin against the CPU on every AMD GPU**, the parity gate, the HIP device tests |
| Does not exercise | SVE2 (Apple cores do not expose it), CUDA, SYCL, HIP, the Python golden gate | Metal, SVE2 on a core without it, GPU twins | Metal, CUDA, HIP, the Python golden gate | Metal, SYCL, HIP, the Python golden gate | Metal, CUDA, SYCL, the Python golden gate |
| You need | a terminal | Docker | Docker and access to the GPU's device node | Docker, the NVIDIA driver and the NVIDIA Container Toolkit | Docker and access to `/dev/kfd` and the GPU's render node |

Each prints what it did and did not exercise inside the report (`not_exercised`).

## A. Native macOS bundle (Apple silicon)

Five commands. Replace `<TESTER-TAG>` with the tag the maintainers give you
(it looks like `tester-20261003-1a2b3c4d`) and `<VERSION>` with the version in the
file name, the `git describe` of the tested commit (for example
`v1.0.0-rc.2-312-g1a2b3c4d`). The maintainer who sends you this page gives you both.

```sh
# 1. Download the archive and its checksum (curl sets no quarantine flag on the files).
curl -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-macos-arm64-<VERSION>.tar.gz
curl -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-macos-arm64-<VERSION>.tar.gz.sha256

# 2. Check the download against the checksum; it prints "OK" and nothing else.
shasum -a 256 -c vmafx-tester-macos-arm64-<VERSION>.tar.gz.sha256

# 3. Unpack into one new directory.
tar -xzf vmafx-tester-macos-arm64-<VERSION>.tar.gz

# 4. Run the report (a few minutes; more with a Metal device, which runs the Metal tests and
#    the parity gate); the JSON goes to report.json, a summary to the terminal.
cd vmafx-tester-macos-arm64-<VERSION> && ./run.sh > report.json

# 5. Look at the verdict before you send anything.
grep -E '"(verdict|cpu_model|hw_model)"' report.json
```

What the run does: it reads files in this directory, starts `build/tools/vmaf`, the
test executables in `tests/` and the parity gate in `tester/gate/` (with the bundled
interpreter), and writes the report. It writes nothing outside the
directory except your temporary directory. It needs no network: `run.sh` starts the
report under macOS's `sandbox-exec` with network access denied when your macOS offers
it, and says so on the terminal; the report program contains no network code in any
case. It never asks for `sudo`.

What the run does not do: it does not install anything, change settings, contact a
server, or read your files outside this directory. The report contains no host name,
user name, serial number or UUID (see [What the report contains](#what-the-report-contains)).

### What is in the bundle

Sizes are approximate; the exact file list with sizes is `bundle-files.txt` in the
workflow run that built it.

| Path | What | Size |
| :--- | :--- | ---: |
| `run.sh` | the one command (about 20 lines of `sh`) | 1 KB |
| `runtime/` | a Python 3.13 interpreter ([python-build-standalone](https://github.com/astral-sh/python-build-standalone), pinned by SHA-256, standard library only) | about 45 MB |
| `tester/` | the report program, plain Python (`tools/rc1-tester/` in the repository) | 0.2 MB |
| `tester/gate/` | the parity gate the report runs on Metal, plain Python (`scripts/ci/cross_backend_parity_gate.py` with its list of exact twins and the decision records that list cites) | 0.5 MB |
| `build/tools/vmaf` | the VMAFx command line tool; libvmaf is linked in, Metal is on, no ONNX Runtime | about 10 MB |
| `tests/` | unit test executables, including the Metal parity tests | about 40 MB |
| `python/test/resource/` | Netflix test videos, each checked against a pinned SHA-256 | about 57 MB |
| `reference/`, `image/` | scores recorded by the build, manifests | under 1 MB |
| `licenses/` | `THIRD_PARTY_NOTICES.txt` and the licence texts of everything above (see [Licences of what you download](#licences-of-what-you-download)) | under 1 MB |

### Remove it afterwards

```sh
cd .. && rm -rf vmafx-tester-macos-arm64-<VERSION> vmafx-tester-macos-arm64-<VERSION>.tar.gz*
```

### Check the download more closely (optional)

The checksum in step 2 comes from the same place as the archive, so it detects a broken
download, not a forged one. Two independent checks tie the archive to the repository's
hosted build. Either needs a tool you may not have; neither is needed to run the bundle.

```sh
# GitHub build provenance (needs the GitHub CLI, `gh`):
gh attestation verify vmafx-tester-macos-arm64-<VERSION>.tar.gz -R VMAFx/vmafx

# Sigstore keyless signature (needs `cosign`; the .bundle file is next to the archive):
cosign verify-blob --bundle vmafx-tester-macos-arm64-<VERSION>.tar.gz.bundle \
  --certificate-identity-regexp '^https://github.com/VMAFx/vmafx/\.github/workflows/macos-tester-bundle\.yml@refs/heads/master$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  vmafx-tester-macos-arm64-<VERSION>.tar.gz

# The SPDX software bill of materials (the .spdx.json asset) is attested on the archive:
gh attestation verify vmafx-tester-macos-arm64-<VERSION>.tar.gz -R VMAFx/vmafx \
  --predicate-type https://spdx.dev/Document/v2.3
```

To read what you are about to run: `run.sh` is in the bundle,
the report program is [`tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py`](https://github.com/VMAFx/vmafx/blob/master/tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py)
and its neighbours `hw_*.py`, and the build is
[`scripts/ci/build-macos-tester-bundle.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/ci/build-macos-tester-bundle.sh)
run by [`macos-tester-bundle.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/macos-tester-bundle.yml).

### Gatekeeper

The binaries carry an ad-hoc code signature (macOS on Apple silicon refuses unsigned
code) and no Apple notarization, because the project has no Apple Developer ID. A file
downloaded with `curl` has no `com.apple.quarantine` attribute, so Gatekeeper does not
check it and the bundle runs. If you downloaded the archive with a browser instead,
macOS marked it as quarantined and will refuse the first run; clear the mark on the
unpacked directory (it only removes that attribute, nothing else) and run again:

```sh
xattr -dr com.apple.quarantine vmafx-tester-macos-arm64-<VERSION>
```

## B. Container image

You need Docker and nothing else. `<VERSION>` is the same string as above. On an Apple silicon Mac Docker Desktop runs a Linux
arm64 virtual machine, so the container tests the fork's aarch64 code on your real CPU.
It cannot reach Metal and SVE2 is not exposed by Apple cores; the report says so.

```sh
# 1. Check the signature of the image (needs `cosign`; skip if you do not have it).
cosign verify \
  --certificate-identity-regexp '^https://github.com/VMAFx/vmafx/\.github/workflows/docker-publish-tester\.yml@refs/(heads/master|tags/v.*)$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  ghcr.io/vmafx/vmafx:<VERSION>-tester

# 2. Pull it and note its digest.
docker pull ghcr.io/vmafx/vmafx:<VERSION>-tester
docker image inspect --format '{{index .RepoDigests 0}}' ghcr.io/vmafx/vmafx:<VERSION>-tester

# 3. Run it. This exact line makes the container unable to reach the network, write
#    its own filesystem, gain privileges or keep any Linux capability.
docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp \
  ghcr.io/vmafx/vmafx:<VERSION>-tester > report.json
```

No volume mount is needed; the report goes to your standard output (`report.json`) and a
short summary to the terminal. The run takes about eight minutes on a fast desktop CPU (most of it the golden gate); allow longer on a laptop. Exit status
0 means everything the run exercised agrees; the report is printed either way.
To record the image digest in the report add `--image-digest sha256:...` after the image
name.

To read what runs: the Containerfile is
[`docker/Dockerfile.tester`](https://github.com/VMAFx/vmafx/blob/master/docker/Dockerfile.tester)
and the report program is `tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py` and its
`hw_*.py` neighbours. The licences of everything in the image are in
`/opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt` inside it (`docker run --rm --entrypoint cat
ghcr.io/vmafx/vmafx:<VERSION>-tester /opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt`); see
[Licences of what you download](#licences-of-what-you-download). The image is built only by
[`docker-publish-tester.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/docker-publish-tester.yml)
from a tagged commit, signed keyless with cosign and attested like the other VMAFx
images. It is about 1.05 GB on disk and about 270 MB to download; it holds a CPU-only build,
no compiler, and runs as a numeric non-root user.

## C. Intel GPU image (Linux, or Windows with WSL2)

You need Docker and an Intel GPU: integrated graphics of an Intel Core processor of
the 11th generation or later (UHD Graphics 7xx, Iris Xe, the Arc graphics of Core Ultra),
an Arc A- or B-series card, or a Data Center GPU. The image holds a SYCL build of
VMAFx compiled ahead of time for all of these, with a portable form for anything newer,
and the Intel GPU runtime it needs. The GPU kernel driver stays your system's own: on
Linux the `i915` or `xe` driver, on Windows the Intel graphics driver.

What the run does on every Intel GPU it finds (at most four), one after the other:

- runs every CPU feature extractor on the four test fixtures with `--backend sycl` at
  full precision and compares each value with the CPU's, per fixture and per feature;
- runs the project's parity gate, `scripts/ci/cross_backend_parity_gate.py`, for every
  SYCL twin on every fixture, compared exactly (ciede at its `1e-9` math-library bound);
- runs the SYCL device tests of the build (69 tests), among them the scratch
  audit `test_sycl_kernel_scratch`, which checks that no kernel on your GPU uses
  scratch memory;
- says which open state rows of the project's bug ledger your GPU's measurements close
  (see [Intel GPU state rows](#intel-gpu-state-rows)).

It also runs the CPU checks of the container image above, except the Python golden gate.
On a desktop with one GPU it takes a few minutes (90 seconds with an Arc A380).

`<VERSION>` is the version the maintainers give you, as for the other packages. The
signature check (`cosign verify ...`) of [B](#b-container-image) works the same way for
`ghcr.io/vmafx/vmafx:<VERSION>-tester-sycl`.

### On Linux

```sh
docker pull ghcr.io/vmafx/vmafx:<VERSION>-tester-sycl

docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp \
  --device /dev/dri $(stat -c '--group-add %g' /dev/dri/renderD* | sort -u) \
  ghcr.io/vmafx/vmafx:<VERSION>-tester-sycl > report.json
```

`--device /dev/dri` gives the container the GPU's device nodes. The container runs as
an unprivileged user (uid 10001), and a render node such as `/dev/dri/renderD128`
usually belongs to the group `render` (on older systems `video`). The `$(stat ...)`
part prints one `--group-add <group id>` per group that owns a render node, so the
container's user may open them; it changes nothing on your system. With rootless
Podman, use `--group-add keep-groups` in its place (the project has not tested
Podman).

### On Windows with WSL2

You need Windows 11, WSL2 (`wsl --update` brings the WSL kernel up to date) and the
current Intel graphics driver for Windows: the
[Intel Arc and Iris Xe driver](https://www.intel.com/content/www/us/en/download/785597/intel-arc-graphics-windows.html)
for Arc GPUs and Core Ultra graphics, the
[11th to 14th generation processor graphics driver](https://www.intel.com/content/www/us/en/download/864990/intel-11th-14th-gen-processor-graphics-windows.html)
for UHD Graphics 7xx and Iris Xe of those generations. That is the setup Intel's
compute runtime documents for WSL2 ([WSL.md](https://github.com/intel/compute-runtime/blob/master/documentation/WSL.md)).
Docker can be Docker Desktop with its WSL integration turned on for your Linux
distribution, or Docker Engine installed inside WSL.

Run this in the WSL Linux shell (for example Ubuntu), not in PowerShell:

```sh
ls -l /dev/dxg /usr/lib/wsl/lib/libdxcore.so    # both must exist

docker pull ghcr.io/vmafx/vmafx:<VERSION>-tester-sycl

docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp \
  --device /dev/dxg -v /usr/lib/wsl:/usr/lib/wsl:ro \
  ghcr.io/vmafx/vmafx:<VERSION>-tester-sycl > report.json
```

Under WSL2 the GPU is `/dev/dxg`, and the user-mode half of your Windows driver is in
`/usr/lib/wsl`, which the container reads but cannot change (`:ro`). The project has
run the compute runtime this image carries under Docker Desktop's WSL2 backend on an
Arc B580 and a UHD 770, but not this image with exactly this command yet: if the report
says `gpu: no_device` or names a problem, send it anyway. The report's `gpu.access`
field records how the container reached the GPU (`wsl`, `drm` or `none`) and why not.

### What to check before you send it

The terminal summary ends with one line per GPU, for example:

```text
gpu (sycl): pass, path drm
  device 0 Intel(R) UHD Graphics 770 (xe-lp 12.2.0): pass; twins identical, gate pass, tests pass (69 passed, 0 failed, 0 skipped), test_sycl_kernel_scratch pass; state rows 1 passing, 0 failing, 0 not measured
```

`gpu (sycl): no_device` with a reason means the container could not reach your GPU;
the reason names the `docker run` option that is missing. Nothing on the GPU was
measured then, which is not a failure, but such a report is only useful to fix the
command. A `fail` on a GPU is a finding: send it.

### Intel GPU state rows

The image carries `image/sycl-rows.json` (in the repository
`tools/rc1-tester/image/sycl-rows.json`): per open SYCL row of
[`docs/state.md`](../state.md) and GPU family, the device tests, audits and gate cells
that close it. The report reads each GPU's family from its IP version (`xe-lp` for UHD
and Iris Xe graphics of the 11th to 14th generation, `xe-lpg` for Core Ultra graphics,
`xe-hpg` for Arc A-series, `xe2` for Arc B-series and Lunar Lake) and gives each row a
verdict for that GPU: `pass`, `fail`, `not_measured`, or `not_applicable` for a row of
another family. The row this image was made for,
`T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02`, needs a UHD 770 or another Xe-LP or
Xe-LPG GPU.

### What is in the Intel GPU image

About 0.95 GB to download and 1.6 GB on disk. Its notices are
`/opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt`, with the licence texts next to it; read
them with
`docker run --rm --entrypoint cat ghcr.io/vmafx/vmafx:<VERSION>-tester-sycl /opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt`
and see [Licences of what you download](#licences-of-what-you-download).

| Path | What | Licence |
| :--- | :--- | :--- |
| `/opt/vmafx/build`, `/opt/vmafx/tests`, `/opt/vmafx/tester` | VMAFx: the `vmaf` tool and library (SYCL build), about a hundred test programs, the report program and the parity gate | EUPL-1.2 and BSD-2-Clause-Patent (Netflix), per file |
| `/opt/vmafx/python/test/resource` | Netflix test videos, each checked against a pinned SHA-256 | BSD-2-Clause-Patent |
| `/opt/vmafx/lib/intel` | Intel's SYCL runtime (`libsycl`, the Unified Runtime loader and its Level Zero adapters, the compiler's math libraries), UMF and hwloc, copied unmodified | Intel End User License Agreement for Developer Tools (these files are its Redistributables: you may not reverse engineer them; see the notices), Apache-2.0 WITH LLVM-exception (UMF), BSD-3-Clause (hwloc) |
| Debian packages | Intel's compute runtime for Level Zero, the Intel Graphics Compiler, gmmlib, the Level Zero loader (all from the pinned [compute-runtime release](https://github.com/intel/compute-runtime/releases/tag/26.35.39758.10)), Python 3.13 and the Debian 13 base | MIT (Intel GPU stack), each package's own (`/usr/share/doc/<package>/copyright`) |

The image holds no compiler, no development package and no GPU kernel driver, and it
cannot reach the network when run with the commands above.

## D. NVIDIA GPU image (Linux, or Windows with WSL2)

You need Docker and an NVIDIA GPU of compute capability 8.0 or newer, in an x86-64
machine. The image holds a CUDA build of VMAFx and runs it on your own NVIDIA driver;
it carries no NVIDIA library.

| Your GPU | Examples | Code the run uses |
| :--- | :--- | :--- |
| Ampere | GeForce RTX 30 series, RTX A2000 to A6000, A100, A10, A30 | the build's `sm_80` or `sm_86` code |
| Ada | GeForce RTX 40 series, RTX 2000 to 6000 Ada, L4, L40 | the build's `sm_89` code |
| Hopper | H100, H200 | the build's `sm_90` code |
| Blackwell | GeForce RTX 50 series, RTX PRO Blackwell, B200, B300 | the build's `sm_100` or `sm_120` code |
| Turing and older | GeForce RTX 20 / GTX 16 series, T4 | not supported (the build starts at 8.0); the report says so |

A newer GPU the build has no code for runs the build's PTX, which your driver compiles
when the run starts; the report names which code each GPU ran. The image is for x86-64
only: Arm machines (Grace Hopper, DGX Spark, Jetson) cannot run it.

What the run does on every NVIDIA GPU it finds (at most four), one after the other:

- runs every CPU feature extractor on the four test fixtures with `--backend cuda` at
  full precision and compares each value with the CPU's, per fixture and per feature;
- runs the project's parity gate for every CUDA twin on every fixture, compared exactly
  (ciede at its `1e-9` math-library bound);
- runs the CUDA device tests of the build (66 tests);
- says which open state rows your GPU's measurements close (see
  [NVIDIA GPU state rows](#nvidia-gpu-state-rows)).

It also runs the CPU checks of the container image, except the Python golden gate. On a
desktop with one GPU it takes about two and a half minutes.

### What you install first

1. The NVIDIA driver, version 580 or newer (the first driver of CUDA 13). A GPU that
   runs the build's PTX (see the table) needs 615 or newer.
2. The
   [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html),
   set up for Docker (`sudo nvidia-ctk runtime configure --runtime=docker`, then
   restart Docker). It lends your driver's libraries to the container at run time.

`<VERSION>` is the version the maintainers give you, as for the other packages. The
signature check (`cosign verify ...`) of [B](#b-container-image) works the same way for
`ghcr.io/vmafx/vmafx:<VERSION>-tester-cuda`.

### On Linux

```sh
docker pull ghcr.io/vmafx/vmafx:<VERSION>-tester-cuda

docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp --gpus all \
  ghcr.io/vmafx/vmafx:<VERSION>-tester-cuda > report.json
```

`--gpus all` asks the NVIDIA Container Toolkit for every GPU and for your driver's
compute libraries. If your toolkit is set up for CDI (`nvidia-ctk cdi generate`), the
same run works with `--device nvidia.com/gpu=all` in place of `--gpus all`; both were
measured. The container runs as an unprivileged user (uid 10001) and needs no group:
the NVIDIA driver opens its device nodes to every user by default.

### On Windows with WSL2 (not yet proven)

You need Windows 11 (or Windows 10 21H2 or later), the current
[NVIDIA Windows driver](https://www.nvidia.com/Download/index.aspx) (it supports WSL2
by itself; do not install a Linux NVIDIA driver inside WSL), and Docker Desktop with
the WSL2 backend turned on, as
[Docker's GPU support page](https://docs.docker.com/desktop/features/gpu/) and
NVIDIA's [CUDA on WSL guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)
describe. Then run, in PowerShell or in the WSL Linux shell:

```sh
docker run --rm --network none --read-only --cap-drop ALL --security-opt no-new-privileges --tmpfs /tmp --gpus all ghcr.io/vmafx/vmafx:<VERSION>-tester-cuda > report.json
```

Docker Desktop passes the GPU as `/dev/dxg` and lends the user-mode half of your
Windows driver (`/usr/lib/wsl/lib/libcuda.so.1`) to the container. The project has not
run this image under WSL2: if the report says `gpu: no_device` or names a problem, send
it anyway. Its `gpu.access.path` field says how the container reached the GPU
(`wsl`, `nvidia` or `none`).

### What to check before you send it

The terminal summary ends with one line per GPU, for example:

```text
gpu (cuda): pass, path nvidia
  device 0 NVIDIA GeForce RTX 4090 (ada 8.9): pass; twins identical, gate pass, tests pass (66 passed, 0 failed, 0 skipped), no audit; state rows 1 passing, 0 failing, 0 not measured
```

`gpu (cuda): no_device` with a reason means the container could not reach your GPU;
the reason names what is missing. A `fail` on a GPU is a finding: send it.

### NVIDIA GPU state rows

The image carries `image/cuda-rows.json` (in the repository
`tools/rc1-tester/image/cuda-rows.json`). Every CUDA twin is proven exact on one GPU
of the project, an RTX 4090 (Ada); the row
`T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03` of [`docs/state.md`](../state.md) stays
open for Ampere, Hopper and Blackwell until a report from such a GPU arrives. The
report reads each GPU's family from its compute capability and gives the row a verdict
for that GPU: `pass`, `fail`, `not_measured`, or `not_applicable` for a part of
another family.

### What is in the NVIDIA GPU image

About 0.5 GB to download and 1.5 GB on disk. Read its notices with
`docker run --rm --entrypoint cat ghcr.io/vmafx/vmafx:<VERSION>-tester-cuda /opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt`
and see [Licences of what you download](#licences-of-what-you-download).

| Path | What | Licence |
| :--- | :--- | :--- |
| `/opt/vmafx/build`, `/opt/vmafx/tests`, `/opt/vmafx/tester` | VMAFx: the `vmaf` tool and library (CUDA build, with its GPU kernels inside), about a hundred test programs, the report program and the parity gate | EUPL-1.2 and BSD-2-Clause-Patent (Netflix), per file; the kernels also hold NVIDIA code under the CUDA Toolkit EULA, and the CUDA driver loader is MIT (`nv-codec-headers`) |
| `/opt/vmafx/python/test/resource` | Netflix test videos, each checked against a pinned SHA-256 | BSD-2-Clause-Patent |
| Debian packages | Python 3.13 and the Debian 13 base | each package's own (`/usr/share/doc/<package>/copyright`) |

The image holds no NVIDIA library, no compiler, no development package and no GPU
driver, and it cannot reach the network when run with the commands above.

## E. AMD GPU image (Linux)

You need Docker, Linux with the kernel's `amdgpu` driver (every current distribution
has it) and an AMD GPU the image has code for. The image holds a HIP build of VMAFx and
the ROCm 10.0.0 runtime files it needs; the GPU kernel driver stays your system's own.

| Your GPU | Examples | gfx targets in the image |
| :--- | :--- | :--- |
| CDNA (Instinct) | MI100, MI210, MI250, MI300X, MI300A, MI325X, MI350X | `gfx908`, `gfx90a`, `gfx942`, `gfx950` |
| RDNA2 | Radeon RX 6800 / 6900, RX 6700, RX 6600, RX 6500, PRO W6800, Radeon 680M, Ryzen 7000 desktop graphics | `gfx1030`, `gfx1031`, `gfx1032`, `gfx1034`, `gfx1035`, `gfx1036` |
| RDNA3 and RDNA3.5 | Radeon RX 7900, RX 7800 / 7700, RX 7600, PRO W7000 series, Radeon 780M, 890M, 8060S | `gfx1100`, `gfx1101`, `gfx1102`, `gfx1103`, `gfx1150`, `gfx1151` |
| RDNA4 | Radeon RX 9070, RX 9060, AI PRO R9700 | `gfx1200`, `gfx1201` |

A GPU whose gfx target is not in the list (older GCN cards, RDNA1) is listed in the
report with the reason and not run. Windows is not supported: ROCm under WSL2 needs
AMD's own WSL runtime, which this image does not carry; a run there reports
`gpu: no_device` and says so.

What the run does on every AMD GPU it finds (at most four), one after the other:

- runs every CPU feature extractor on the four test fixtures with `--backend hip` at
  full precision and compares each value with the CPU's, per fixture and per feature
  (`adm` and `float_vif` keep their CPU extractor in that run; the gate below measures
  their HIP twins);
- runs the project's parity gate for every HIP twin on every fixture, compared exactly
  (ciede at its `1e-9` math-library bound);
- runs the HIP device tests of the build (73 tests);
- says which open state rows your GPU's measurements close (see
  [AMD GPU state rows](#amd-gpu-state-rows)).

It also runs the CPU checks of the container image, except the Python golden gate.
On a desktop with one GPU it takes about two minutes.

`<VERSION>` is the version the maintainers give you, as for the other packages. The
signature check (`cosign verify ...`) of [B](#b-container-image) works the same way for
`ghcr.io/vmafx/vmafx:<VERSION>-tester-hip`.

### On Linux

```sh
docker pull ghcr.io/vmafx/vmafx:<VERSION>-tester-hip

docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp \
  --device /dev/kfd --device /dev/dri \
  $(stat -c '--group-add %g' /dev/kfd /dev/dri/renderD* | sort -u) \
  ghcr.io/vmafx/vmafx:<VERSION>-tester-hip > report.json
```

`/dev/kfd` is ROCm's compute device and `/dev/dri` holds the GPU's render nodes. The
container runs as an unprivileged user (uid 10001), and these nodes usually belong to
the group `render` (on some systems `video`). The `$(stat ...)` part prints one
`--group-add <group id>` per group that owns them, so the container's user may open
them; it changes nothing on your system. With rootless Podman, use
`--group-add keep-groups` in its place (the project has not tested Podman).

### What to check before you send it

The terminal summary ends with one line per GPU, for example:

```text
gpu (hip): pass, path kfd
  device 0 AMD Radeon Graphics (rdna2 gfx1036): pass; twins identical, gate pass, tests pass (73 passed, 0 failed, 0 skipped), no audit; state rows 1 passing, 0 failing, 0 not measured
```

`gpu (hip): no_device` with a reason means the container could not reach your GPU; the
reason names the missing `docker run` option. A `fail` on a GPU is a finding: send it.

### AMD GPU state rows

The image carries `image/hip-rows.json` (in the repository
`tools/rc1-tester/image/hip-rows.json`). Every HIP twin is proven exact on one GPU of
the project, the small RDNA2 graphics of a Ryzen 7000 desktop processor (`gfx1036`);
the row `T-HIP-TWINS-OTHER-TARGETS-2026-10-03` of [`docs/state.md`](../state.md) stays
open for CDNA, RDNA3, RDNA3.5 and RDNA4 until a report from such a GPU arrives. The
report reads each GPU's family from its gfx target and gives the row a verdict for
that GPU: `pass`, `fail`, `not_measured`, or `not_applicable` for a part of another
family.

### What is in the AMD GPU image

About 0.6 GB to download and 1.9 GB on disk. Read its notices with
`docker run --rm --entrypoint cat ghcr.io/vmafx/vmafx:<VERSION>-tester-hip /opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt`
and see [Licences of what you download](#licences-of-what-you-download).

| Path | What | Licence |
| :--- | :--- | :--- |
| `/opt/vmafx/build`, `/opt/vmafx/tests`, `/opt/vmafx/tester` | VMAFx: the `vmaf` tool and library (HIP build, with its GPU code objects inside), about a hundred test programs, the report program and the parity gate | EUPL-1.2 and BSD-2-Clause-Patent (Netflix), per file |
| `/opt/vmafx/python/test/resource` | Netflix test videos, each checked against a pinned SHA-256 | BSD-2-Clause-Patent |
| `/opt/vmafx/lib/rocm` | the ROCm 10.0.0 runtime the HIP build loads, copied unmodified: the HIP runtime (`libamdhip64`), the ROCm runtime (`libhsa-runtime64`), the code object manager (`libamd_comgr`) with the LLVM and Clang libraries it links, `rocprofiler-register`, `kpack`, and the system libraries ROCm bundles (`rocm_sysdeps`) | MIT (HIP runtime, rocprofiler-register, kpack, libdrm), NCSA (ROCm runtime), Apache-2.0 WITH LLVM-exception (comgr, LLVM, Clang), LGPL (libelf, libnuma), Zlib, BSD-3-Clause (zstd), 0BSD (liblzma), bzip2 |
| Debian packages | Python 3.13 and the Debian 13 base | each package's own (`/usr/share/doc/<package>/copyright`) |

The image holds no compiler, no development package and no GPU driver, and it cannot
reach the network when run with the commands above.

## Licences of what you download

Every package carries the licence of everything in it, and its publishing
workflow refuses to build a package with a file whose licence is not recorded
([ADR-1503](../adr/1503-tester-artifact-licensing.md)).

- **Where**: `licenses/THIRD_PARTY_NOTICES.txt` in the macOS bundle,
  `/opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt` in the container images. The file lists
  every component, its licence and copyright notices, and the licence texts are in
  `texts/` next to it. Debian packages in the container keep their own terms in
  `/usr/share/doc/<package>/copyright`; Python packages keep theirs in their
  `*.dist-info` directories.
- **VMAFx itself** (`vmaf`, libvmaf, the tests, the report program): the fork's own
  code is under EUPL-1.2 and the code it carries from others keeps its terms:
  BSD-2-Clause-Patent (Netflix's VMAF), BSD-3-Clause (IQA, libsvm, the JPEG XL
  project's SSIMULACRA 2), BSD-2-Clause (Xiph, Daala, dav1d), ISC (x264's assembly
  macros, x86 only), MIT (CIEDE2000, mkdirp) and the Unlicense (pdjson); the
  container's Python harness adds files under BSD-3-Clause-Clear. The
  built-in BRISQUE model is the LIVE laboratory's release, under its notice in
  `texts/LicenseRef-LIVE-BRISQUE.txt`: use "for any purpose, provided that the
  copyright notice in its entirety appear in all copies", with an
  acknowledgement and citation in publications that report research using it. The notices name the exact source commit;
  the source is the repository at that commit.
- **The test videos** come from `Netflix/vmaf_resource` under BSD-2-Clause-Patent.
- **The interpreter** is CPython under the PSF licence; the notices add the
  licences of the libraries linked into it (OpenSSL under Apache-2.0, libffi,
  expat, mpdecimal, bzip2, HACL\* and others).
- **Container only**: the Debian base system includes GPL and LGPL programs and
  libraries, and the numpy and scipy wheels include LGPL `libquadmath`. Their
  corresponding source is published next to the image as
  `ghcr.io/vmafx/vmafx:<VERSION>-tester-source`: Debian source packages at the
  installed versions and the source RPMs of the wheels' GCC runtime libraries,
  indexed by `/sources/SOURCES.txt`. To get it:
  `docker create --name vmafx-src ghcr.io/vmafx/vmafx:<VERSION>-tester-source true`,
  `docker cp vmafx-src:/sources ./vmafx-tester-sources`, `docker rm vmafx-src`.
- **Intel GPU image only**: Intel's SYCL runtime files are Redistributables of the
  Intel End User License Agreement for Developer Tools; its text, the compiler's
  `third-party-programs.txt` and its list of Redistributables (`credist.txt`) are in
  `/opt/vmafx/licenses/intel/`, and the notices state the terms that agreement passes
  on to you (executable code only, no reverse engineering, its limitation of
  liability). UMF is under Apache-2.0 WITH LLVM-exception, hwloc under BSD-3-Clause.
  The Intel GPU stack is MIT (the Intel Graphics Compiler with LLVM parts under
  Apache-2.0 WITH LLVM-exception); the notices name its release and source. The
  image's interpreter is Debian's Python 3.13, under the PSF licence in its package's
  copyright file. The source of its Debian packages is
  `ghcr.io/vmafx/vmafx:<VERSION>-tester-sycl-source`, fetched the same way.
- **NVIDIA GPU image only**: the image holds no NVIDIA file. The driver library the
  run uses (`libcuda.so.1`) comes from your own NVIDIA driver, which the NVIDIA
  Container Toolkit (or WSL2) lends to the container, under the driver's licence.
  Part of the VMAFx program code is NVIDIA's: the GPU kernels inside `libvmaf` and the
  test programs contain code from the CUDA Toolkit's headers and its `libdevice` maths
  library, which the
  [CUDA Toolkit End User License Agreement](https://docs.nvidia.com/cuda/eula/index.html)
  (v13.4, Attachment A) allows a program to carry. Its text is
  `/opt/vmafx/licenses/nvidia/CUDA-EULA.txt`, and the notices state the terms it
  passes on to you (no reverse engineering of those parts; NVIDIA gives them as is).
  EUPL-1.2 covers only the VMAFx files, never NVIDIA's. The CUDA driver loader that
  `libvmaf` compiles in comes from FFmpeg's `nv-codec-headers` (MIT), whose notices are
  in `/opt/vmafx/licenses/nv-codec-headers/`. The source of the image's Debian
  packages is `ghcr.io/vmafx/vmafx:<VERSION>-tester-cuda-source`.
- **AMD GPU image only**: the ROCm 10.0.0 runtime files in `/opt/vmafx/lib/rocm` are
  open source and ship unmodified: the HIP runtime, `rocprofiler-register` and `kpack`
  under MIT, the ROCm runtime under NCSA, the code object manager and the LLVM and Clang
  libraries it links under Apache-2.0 WITH LLVM-exception, and the system libraries ROCm
  bundles under their own licences (libdrm MIT, zlib, zstd BSD-3-Clause, liblzma 0BSD,
  bzip2). Their texts are in `/opt/vmafx/licenses/rocm/` and `/opt/vmafx/licenses/texts/`.
  Two of the bundled libraries are LGPL (elfutils' `libelf`, numactl's `libnuma`): you
  may replace them, and their corresponding source (the upstream archives and the
  TheRock tree that built them) is in `ghcr.io/vmafx/vmafx:<VERSION>-tester-hip-source`
  with the source of the image's Debian packages.
- **SBOM**: each package has an SPDX software bill of materials attested by the
  publishing workflow: the `.spdx.json` release asset for the macOS bundle, and an
  attestation on each platform image of the container
  (`gh attestation verify oci://ghcr.io/vmafx/vmafx@<platform digest> -R VMAFx/vmafx
  --predicate-type https://spdx.dev/Document/v2.3`, with the platform digest from
  `docker buildx imagetools inspect ghcr.io/vmafx/vmafx:<VERSION>-tester`), and an
  attestation on the digest of each GPU image.

## What the report contains

One JSON document (schema: [`docs/hardware-reports/report.schema.json`](../hardware-reports/report.schema.json)):

- **Host facts**: CPU model string, the allow-listed `cpuinfo` / `sysctl` keys, CPU
  feature flags, `AT_HWCAP` / `AT_HWCAP2` on Linux, the dispatch flags the fork selects
  (NEON always on arm64, SVE2 only when the kernel reports it, as `core/src/arm/cpu.c`
  does), kernel release, macOS version and hardware model identifier (for example
  `Mac16,1`) and the Metal device name and family on a Mac.
- **Dispatch equivalence**: every CPU feature extractor on four fixtures at
  `--precision max` (`%.17g`), default dispatch against scalar C: identical and
  differing value counts per fixture and, for a difference, metric, first frame and
  both values.
- **Reference equivalence** (container): the same scores against reference scores the
  image build recorded with its own binary. A difference here is the interesting
  finding.
- **Metal equivalence** (macOS bundle): every CPU extractor with `--backend metal`
  against the CPU, with the extractors that really ran on Metal, counts, first
  differing frame, both values and the largest absolute difference.
- **Metal parity gate** (macOS bundle, `metal_gate`): the project's parity gate,
  `scripts/ci/cross_backend_parity_gate.py`, run on every fixture with
  `--backends cpu metal --hold-exact metal`: every Metal twin against its CPU
  extractor, compared exactly at full precision (ciede at its `1e-9` math-library
  bound), with the option sets the gate has cells for (`enable_lcs`, `debug`, the
  five-frame motion window). A feature a Metal twin cannot run on a fixture is
  listed under `left_out` with the reason.
- **GPU section** (GPU images, `gpu`): how the container reached the GPU
  (`access.path`: `drm` for a Linux render node, `nvidia` for the NVIDIA device nodes,
  `kfd` for ROCm's `/dev/kfd`, `wsl` for WSL2's `/dev/dxg`, `none` with the reason), the versions of the GPU
  runtime in the image (the NVIDIA image: the CUDA version of your driver and of the
  build), and per GPU its name and family with, for Intel, the PCI device ID, IP
  version, execution units and sub-group sizes, for NVIDIA the compute capability,
  multiprocessor count, memory size and the kernel code it ran (a cubin of the build
  or PTX your driver compiled), for AMD the gfx target, compute units, clock and PCI
  device ID; the twins against the CPU per fixture and per feature (identical values, values
  within the gate's bound for that twin, or the first differing value with both
  numbers); the parity gate's cells; every device test's verdict and the ones left
  out with the reason; on Intel GPUs the scratch audit (kernels audited, kernels in
  scratch memory, whether your GPU returns wrong values from scratch memory); and the
  state rows the GPU's measurements close.
- **Unit tests** and, in the container, the **Netflix golden gate**: passed, failed,
  skipped, names of failures. For the Metal parity tests the report also keeps the
  verdict of every test case (`unit_tests.cases`) and the message of a failing one.
- **Metal state rows** (macOS bundle, `metal_rows`): for every open Metal row of the
  project's bug ledger, the cases, fixture scores and gate cells of this run that
  close it, and a verdict: `pass` (every one of them passed on this Mac), `fail`, or
  `not_measured` (for example no Metal device). See
  [Metal state rows](#metal-state-rows).
- **What was not exercised and why**, the source commit, tool versions and a schema
  version.

It does not contain: a host name, user name, home directory, serial number, UUID, MAC
address, IP address, PCI bus address or any network identifier. The CPU model and hardware model
identifier are the only things that identify your machine's kind. Read `report.json`
before you send it; it is plain text.

Exit status 0 means every check that ran agreed. A Metal run on a Mac with no usable
Metal device is reported as `no_device` and is not a failure. A report with verdict
`fail` is as welcome as a passing one: it is the finding.

### Metal state rows

The macOS report says, row by row, which open Metal defects of
[`docs/state.md`](../state.md) this run measured. The map from a row to its
measurements is `image/metal-rows.json` in the bundle (in the repository
`tools/rc1-tester/image/metal-rows.json`): test cases of the Metal parity tests,
which compare a Metal twin with its CPU extractor with `==` on synthetic fixtures
(16-bit full-range noise, frames below 17 pixels, option sets, identical pairs),
Metal scores on the four fixtures, and cells of the parity gate. A row whose
verdict is `pass` has every measurement it names passing on this Mac; the
maintainers close it from that evidence. The summary on the terminal prints the
counts (`metal state rows: N measured passing, ...`).

On a Mac without a usable Metal device every Metal case is skipped, the gate is
`no_device` and every row is `not_measured`; that is not a failure.

## Send the report and get credit

### As a pull request (credited as the commit author)

Your commit carries your own git identity, so GitHub lists you as a contributor. The
run prints the file name to use (`file name for a pull request: ...`). With the GitHub
CLI (`gh`) and `git` set up with your name and e-mail address:

```sh
gh repo fork VMAFx/vmafx --clone && cd vmafx
git switch -c hardware-report && cp ../report.json <file name printed by the run>
git add docs/hardware-reports && git commit -m "docs(hardware-reports): add a report from <your CPU>"
git push -u origin hardware-report && gh pr create --repo VMAFx/vmafx --fill
```

Edit the `"note"` field of the JSON first if you want to add a line (for example the Mac
model and your Docker version); nothing else may be edited, because CI checks the
integrity hash the tool wrote and refuses a hand-edited file. A maintainer regenerates
the index page when the pull request is merged.

### Without opening a pull request

Open an issue with the
[hardware report form](https://github.com/VMAFx/vmafx/issues/new?template=hardware_report.yml)
and attach `report.json`. A maintainer commits it for you; give a name and an e-mail
address in the form if you want a `Co-authored-by:` line with your credit.

Reports from outside the project's own hosts are listed on the
[hardware reports page](../hardware-reports/index.md).

## If something goes wrong

- `run.sh: this bundle is for macOS on Apple silicon` — the bundle is arm64-only; use
  the container on other machines.
- macOS says the developer cannot be verified — see [Gatekeeper](#gatekeeper).
- The report ends `verdict: fail` — that is a result, send it.
- The Intel GPU image says `gpu (sycl): no_device` — the container could not open your
  GPU; the reason names the missing `docker run` option (see
  [C](#c-intel-gpu-image-linux-or-windows-with-wsl2)).
- The NVIDIA GPU image says `gpu (cuda): no_device` — the container could not reach
  your GPU or its driver; the reason says which (no device node: `--gpus all` is
  missing or the NVIDIA Container Toolkit is not set up; no `libcuda.so.1`: the toolkit
  did not lend the driver; a GPU below compute capability 8.0) (see
  [D](#d-nvidia-gpu-image-linux-or-windows-with-wsl2)).
- The AMD GPU image says `gpu (hip): no_device` — the container could not open
  `/dev/kfd` or the render node, or your GPU's gfx target is not in the image; the
  reason says which and names the missing `docker run` option (see
  [E](#e-amd-gpu-image-linux)).
- Anything that stops before a report is printed — send the terminal output in an issue.
