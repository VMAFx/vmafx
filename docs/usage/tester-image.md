<!-- markdownlint-disable MD013 MD024 MD046 -->
# Test VMAFx on your hardware without building anything

This page is for someone who has a machine the project does not own (an Apple
M-series Mac, an Intel GPU, an Ampere or Graviton box, an unusual x86 CPU) and wants
to check the fork on it. You build nothing, install no toolchain and need no
repository checkout. You run one prepared package, it prints one JSON report, and you
can send that report to the project and be credited for it.

Not sure whether your hardware is wanted? [Hardware we need](hardware-we-need.md) lists
every family the project has no report from yet, and which package tests it.

There are six packages. On a Mac, run the **native bundle** first: it also exercises
the Metal backend, which no container can reach. On a Windows PC, run the **Windows
zip**: it tests the build Microsoft's compiler makes, natively, on x64 or Arm64. The
**container image** tests the CPU code paths and works on any machine with Docker. The
**Intel GPU image** tests the
SYCL backend on an Intel GPU (integrated UHD or Iris Xe graphics, Arc, Data Center),
on Linux or on Windows with WSL2. The **NVIDIA GPU image** tests the CUDA backend on
an NVIDIA GPU (GeForce RTX 30 series or newer, RTX professional cards, A100, H100,
B200). The **AMD GPU image** tests the HIP backend on an AMD GPU (Radeon RX 6000, 7000
and 9000 series, Ryzen graphics, Instinct), on Linux.

| | Native macOS bundle | Container image | Intel GPU image | NVIDIA GPU image | AMD GPU image | Native Windows zip |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Runs on | macOS on Apple silicon | any Docker host (Linux arm64 or amd64, Docker Desktop 4.19 or later) | Linux x86-64 with an Intel GPU, or Windows 11 with WSL2 | Linux x86-64 with an NVIDIA GPU, or Windows with WSL2 (not yet proven) | Linux x86-64 with an AMD GPU (not Windows) | Windows 10 (2004 or later) or 11, on x64 or Arm64; the x64 CUDA zip with an NVIDIA GPU |
| Exercises | NEON default dispatch against scalar, **every Metal twin against the CPU**, SIMD unit tests | NEON (or AVX2 / AVX-512) default dispatch against scalar and against baked references, SIMD unit tests, the Netflix golden gate | AVX2 / AVX-512 default dispatch against scalar, **every SYCL twin against the CPU on every Intel GPU**, the parity gate, the SYCL device tests and the scratch-memory audit | AVX2 / AVX-512 default dispatch against scalar, **every CUDA twin against the CPU on every NVIDIA GPU**, the parity gate, the CUDA device tests | AVX2 / AVX-512 default dispatch against scalar, **every HIP twin against the CPU on every AMD GPU**, the parity gate, the HIP device tests | **the MSVC build's** AVX2 / AVX-512 (or NEON) default dispatch against scalar and against baked references, SIMD unit tests and the Windows-only unit tests; the CUDA zip adds **every CUDA twin against the CPU on every NVIDIA GPU**, the parity gate and the CUDA device tests |
| Does not exercise | SVE2 (Apple cores do not expose it), CUDA, SYCL, HIP, the Python golden gate | Metal, SVE2 on a core without it, GPU twins | Metal, CUDA, HIP, the Python golden gate | Metal, SYCL, HIP, the Python golden gate | Metal, CUDA, SYCL, the Python golden gate | SYCL and HIP twins (CUDA in the CUDA zip only), Metal, SVE2, the Python golden gate |
| You need | a terminal | Docker 23.0 or later | Docker 23.0 or later and access to the GPU's device node | Docker 23.0 or later, the NVIDIA driver and the NVIDIA Container Toolkit | Docker 23.0 or later and access to `/dev/kfd` and the GPU's render node | PowerShell or the Command Prompt |

Each prints what it did and did not exercise inside the report (`not_exercised`).

```figure
tester-kit-flow
```

## A. Native macOS bundle (Apple silicon)

Five commands. Replace `<TESTER-TAG>` with the tag the maintainers give you
(it looks like `tester-20261003-1a2b3c4d`) and `<VERSION>` with the version in the
file name, the `git describe` of the tested commit (for example
`v1.0.0-rc.2-312-g1a2b3c4d`). The maintainer who sends you this page gives you both.

```sh
# 1. Download the archive and its checksum (curl sets no quarantine flag on the files).
curl -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-macos-arm64-<VERSION>.tar.xz
curl -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-macos-arm64-<VERSION>.tar.xz.sha256

# 2. Check the download against the checksum; it prints "OK" and nothing else.
shasum -a 256 -c vmafx-tester-macos-arm64-<VERSION>.tar.xz.sha256

# 3. Unpack into one new directory (macOS's own tar reads xz; nothing to install).
tar -xf vmafx-tester-macos-arm64-<VERSION>.tar.xz

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
workflow run that built it. The archive is about 27 MB to download and unpacks to about
185 MB. It is compressed with xz at its strongest level, which finds the
frame-to-frame redundancy of the test videos; the `.tar.gz` of earlier bundles was
about 70 MB.

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
cd .. && rm -rf vmafx-tester-macos-arm64-<VERSION> vmafx-tester-macos-arm64-<VERSION>.tar.xz*
```

### Check the download more closely (optional)

The checksum in step 2 comes from the same place as the archive, so it detects a broken
download, not a forged one. Two independent checks tie the archive to the repository's
hosted build. Either needs a tool you may not have; neither is needed to run the bundle.
To also tie it to VMAFx's GitHub account and not only its name, add the owner-ID
check from [the signer's owner](../development/release.md#the-signers-owner-not-only-its-name)
to the `gh attestation verify` line.

```sh
# GitHub build provenance (needs the GitHub CLI, `gh`):
gh attestation verify vmafx-tester-macos-arm64-<VERSION>.tar.xz -R VMAFx/vmafx

# Sigstore keyless signature (needs `cosign`; the .sigstore.json file is next to the archive):
cosign verify-blob --bundle vmafx-tester-macos-arm64-<VERSION>.tar.xz.sigstore.json \
  --certificate-identity-regexp '^https://github\.com/VMAFx/vmafx/\.github/workflows/macos-tester-bundle\.yml@refs/heads/master$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  vmafx-tester-macos-arm64-<VERSION>.tar.xz

# The SPDX software bill of materials (the .spdx.json asset) is attested on the archive:
gh attestation verify vmafx-tester-macos-arm64-<VERSION>.tar.xz -R VMAFx/vmafx \
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

You need Docker Engine 23.0 or later (Docker Desktop 4.19 or later) and nothing else; Podman
works too. `<VERSION>` is the same string as above. On an Apple silicon Mac Docker Desktop runs a Linux
arm64 virtual machine, so the container tests the fork's aarch64 code on your real CPU.
It cannot reach Metal and SVE2 is not exposed by Apple cores; the report says so.

```sh
# 1. Check the signature of the image (needs `cosign`; skip if you do not have it).
cosign verify \
  --certificate-identity-regexp '^https://github\.com/VMAFx/vmafx/\.github/workflows/docker-publish-tester\.yml@refs/(heads/master|tags/v[0-9][0-9A-Za-z.+-]*)$' \
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
no compiler, and runs as a numeric non-root user. Its layers are zstd-compressed, so an
older Docker stops the pull with `failed to register layer: ... archive/tar: invalid tar
header`; check `docker version --format '{{.Server.Version}}'` and see
[What can pull the images](docker.md#what-can-pull-the-images). Debian 12's own
`docker.io` (20.10) is too old; Docker's own packages, Ubuntu's `docker.io` and Podman work.

## C. Intel GPU image (Linux, or Windows with WSL2)

You need Docker 23.0 or later ([why](docker.md#what-can-pull-the-images)) and an Intel GPU: integrated graphics of an Intel Core processor of
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
Docker can be Docker Desktop 4.19 or later with its WSL integration turned on for your
Linux distribution, or Docker Engine 23.0 or later installed inside WSL.

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

About 0.85 GB to download and 1.35 GB on disk. The GPU code in the library and the test programs is stored compressed and decompressed when it loads ([ADR-1590](../adr/1590-device-code-compression.md)). Its notices are
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

You need Docker 23.0 or later ([why](docker.md#what-can-pull-the-images)) and an NVIDIA GPU of compute capability 8.0 or newer, in an x86-64
machine. The image holds a CUDA build of VMAFx and runs it on your own NVIDIA driver;
it carries no NVIDIA library. On Windows without Docker, the
[Windows CUDA zip](#with-an-nvidia-gpu-the-cuda-zip) runs the Windows build of the
same twins natively.

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
by itself; do not install a Linux NVIDIA driver inside WSL), and Docker Desktop 4.19 or later
with the WSL2 backend turned on, as
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

About 0.45 GB to download and 0.75 GB on disk. The GPU code in the library and the test programs is stored compressed and decompressed when it loads ([ADR-1590](../adr/1590-device-code-compression.md)). Read its notices with
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

You need Docker 23.0 or later ([why](docker.md#what-can-pull-the-images)), Linux with the kernel's `amdgpu` driver (every current distribution
has it) and an AMD GPU the image has code for. The image holds a HIP build of VMAFx and
the ROCm 10.1.0 runtime files it needs; the GPU kernel driver stays your system's own.

| Your GPU | Examples | gfx targets in the image |
| :--- | :--- | :--- |
| CDNA (Instinct) | MI100, MI210, MI250, MI300X, MI300A, MI325X, MI350X | `gfx908`, `gfx90a`, `gfx942`, `gfx950` |
| RDNA1 | Radeon RX 5700, RX 5600, RX 5500, Pro V520 | `gfx1010`, `gfx1011`, `gfx1012` |
| RDNA2 | Radeon RX 6800 / 6900, RX 6700, RX 6600, RX 6500, PRO W6800, Steam Deck, Radeon 680M, Ryzen 7000 desktop graphics | `gfx1030`, `gfx1031`, `gfx1032`, `gfx1033`, `gfx1034`, `gfx1035`, `gfx1036` |
| RDNA3 and RDNA3.5 | Radeon RX 7900, RX 7800 / 7700, RX 7600, PRO W7000 series, Radeon 780M, 860M, 890M, 8060S | `gfx1100`, `gfx1101`, `gfx1102`, `gfx1103`, `gfx1150`, `gfx1151`, `gfx1152`, `gfx1153` |
| RDNA4 and RDNA 4m | Radeon RX 9070, RX 9060, AI PRO R9700 (`gfx1250`: products not yet announced) | `gfx1200`, `gfx1201`, `gfx1250` |

These are all 25 targets ROCm 10.1.0 builds its own libraries for. A GPU whose gfx
target is not in the list (older GCN cards such as the Radeon VII or the RX 500
series) is listed in the report with the reason and not run. Windows is not supported: ROCm under WSL2 needs
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
open for CDNA, RDNA1, RDNA3, RDNA3.5 and RDNA4 until a report from such a GPU arrives. The
report reads each GPU's family from its gfx target and gives the row a verdict for
that GPU: `pass`, `fail`, `not_measured`, or `not_applicable` for a part of another
family.

### What is in the AMD GPU image

About 0.35 GB to download and 0.7 GB on disk. The GPU code in the library and the test programs is stored compressed and decompressed when it loads ([ADR-1590](../adr/1590-device-code-compression.md)). Read its notices with
`docker run --rm --entrypoint cat ghcr.io/vmafx/vmafx:<VERSION>-tester-hip /opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt`
and see [Licences of what you download](#licences-of-what-you-download).

| Path | What | Licence |
| :--- | :--- | :--- |
| `/opt/vmafx/build`, `/opt/vmafx/tests`, `/opt/vmafx/tester` | VMAFx: the `vmaf` tool and library (HIP build, with its GPU code objects inside), about a hundred test programs, the report program and the parity gate | EUPL-1.2 and BSD-2-Clause-Patent (Netflix), per file |
| `/opt/vmafx/python/test/resource` | Netflix test videos, each checked against a pinned SHA-256 | BSD-2-Clause-Patent |
| `/opt/vmafx/lib/rocm` | the ROCm 10.1.0 runtime the HIP build loads, copied unmodified: the HIP runtime (`libamdhip64`), the ROCm runtime (`libhsa-runtime64`), the code object manager (`libamd_comgr`) with the LLVM and Clang libraries it links, `rocprofiler-register`, `kpack`, and the system libraries ROCm bundles (`rocm_sysdeps`) | MIT (HIP runtime, rocprofiler-register, kpack, libdrm), NCSA (ROCm runtime), Apache-2.0 WITH LLVM-exception (comgr, LLVM, Clang), LGPL (libelf, libnuma), Zlib, BSD-3-Clause (zstd), 0BSD (liblzma), bzip2 |
| Debian packages | Python 3.13 and the Debian 13 base | each package's own (`/usr/share/doc/<package>/copyright`) |

The image holds no compiler, no development package and no GPU driver, and it cannot
reach the network when run with the commands above.

## F. Native Windows zip (x64 or Arm64)

You need Windows 10 (version 2004 or later) or Windows 11, PowerShell or the Command
Prompt, and about 1 GB of free disk space (up to 3 GB for the CUDA or SYCL zip). You
install nothing: the zip carries the VMAFx programs and the Python interpreter that runs
the report, and the programs need no Visual C++ runtime on your machine.

There is one zip per processor type. Take `x64` for an Intel or AMD processor and
`arm64` for Windows on Arm (Snapdragon X and other Arm laptops). If you are not sure,
run `echo $env:PROCESSOR_ARCHITECTURE` in PowerShell: `AMD64` means x64, `ARM64` means
arm64. The x64 zip refuses to start on an Arm machine, because it would run under
emulation and measure the emulator.

Five steps in PowerShell. Replace `<TESTER-TAG>` and `<VERSION>` with what the
maintainer gives you (the tag looks like `tester-windows-20261004-1a2b3c4d`), and
`<ARCH>` with `x64` or `arm64`:

```powershell
# 1. Download the zip and its checksum. curl.exe comes with Windows 10 and 11, and
#    a file it downloads carries no "downloaded from the internet" mark.
curl.exe -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-windows-<ARCH>-<VERSION>.zip
curl.exe -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-windows-<ARCH>-<VERSION>.zip.sha256

# 2. Check the download against the checksum; it prints True and nothing else.
(Get-FileHash vmafx-tester-windows-<ARCH>-<VERSION>.zip).Hash -eq (Get-Content vmafx-tester-windows-<ARCH>-<VERSION>.zip.sha256).Split(' ')[0]

# 3. Unpack into one new folder (tar.exe comes with Windows 10 and 11).
tar -xf vmafx-tester-windows-<ARCH>-<VERSION>.zip

# 4. Run the report (a few minutes). It writes report.json into this folder and a
#    summary to the window.
cd vmafx-tester-windows-<ARCH>-<VERSION>; .\run.cmd

# 5. Look at the verdict before you send anything.
Select-String -Path report.json -Pattern '"(verdict|cpu_model|os_version)"'
```

In the Command Prompt the steps are the same with two differences: check the download
with `certutil -hashfile vmafx-tester-windows-<ARCH>-<VERSION>.zip SHA256` and compare
the printed value with the first word of the `.sha256` file by eye, and run `run.cmd`
instead of `.\run.cmd`.

What the run does: it reads the files of the folder, starts `build\tools\vmaf.exe` and
the test programs in `tests\` with the interpreter in `runtime\`, and writes
`report.json` into the folder you ran it from. It writes nothing else outside the
folder except your temporary folder, needs no administrator rights and reads one
registry key, the one where Windows describes your processor. Windows has no
equivalent of the macOS sandbox a user can start without administrator rights, so the
run is not cut off from the network; the report program contains no network code.

What the run does not do: install anything, change a setting, contact a server, or read
your files outside the folder. The report contains no host name, user name, serial
number or UUID.

### What runs on Windows

| Check | What it does | x64 zip | arm64 zip |
| :--- | :--- | :--- | :--- |
| Dispatch equivalence | every CPU feature extractor on four test videos at full precision, your processor's SIMD code against plain C, compared exactly | AVX2, and AVX-512 when your processor has it | NEON |
| Reference equivalence | the same scores against scores the same build recorded on GitHub's runner | yes | yes, without the x86_64 cross-check |
| Unit tests | the SIMD and dispatch tests, and the tests of what only a Windows build has: the Windows thread and option-parsing shims, UTF-8 file names, temporary files, locales | about 57 | those that exist for Arm64 |
| CUDA twins | every CUDA twin against the CPU, the parity gate, the CUDA device tests | the CUDA zip only | no |
| SYCL twins | every SYCL twin against the CPU, the parity gate, the SYCL device tests and the scratch-memory audit | the SYCL zip only | no |
| Not run | HIP twins, Metal, the Python golden gate, ONNX Runtime, SVE2 | | |

The programs are built with Microsoft's compiler (MSVC), the build most Windows users
of VMAFx make. No tester has run that build on his own machine before, and nobody has
compared its AVX2 and AVX-512 code with the plain C code yet, so a `fail` from this zip
is especially useful.

### With an NVIDIA GPU: the CUDA zip

If your PC has an NVIDIA GPU of the GeForce RTX 30 series or newer (or an RTX
professional card), take the third zip, `vmafx-tester-windows-x64-cuda-<VERSION>.zip`,
instead of the x64 one: replace `<ARCH>` with `x64-cuda` in the five steps above. You
need the NVIDIA display driver, version 580 or later, which most gaming and workstation
PCs already have; the zip contains no NVIDIA file and uses the driver's `nvcuda.dll`.

On top of everything the x64 zip runs, the CUDA zip, on every NVIDIA GPU it finds (at
most four), one after the other:

- runs every CPU feature extractor on the four test videos with `--backend cuda` at
  full precision and compares each value with the CPU's;
- runs the project's parity gate for every CUDA twin on every fixture, compared exactly
  (ciede at its `1e-9` math-library bound);
- runs the CUDA device tests of the build;
- says which open state rows your GPU's measurements close
  ([NVIDIA GPU state rows](#nvidia-gpu-state-rows)).

No one has run the Windows CUDA build on a GPU before: GitHub's build machines have
none. A GPU below the RTX 30 series (compute capability below 8.0) is listed in the
report with the reason and not run. Without the driver the report says
`gpu (cuda): no_device` and names `nvcuda.dll`; that is not a failure.

### With an Intel GPU: the SYCL zip

If your PC has an Intel GPU (an Arc A- or B-series card, or the Iris Xe, UHD or Arc
graphics of an 11th-generation Core processor or newer), take the fourth zip,
`vmafx-tester-windows-x64-sycl-<VERSION>.zip`, instead of the x64 one: replace `<ARCH>`
with `x64-sycl` in the five steps above. You need Intel's graphics driver, which Windows
Update or Intel's Driver & Support Assistant installs; the zip reaches the GPU through
the driver's Level Zero driver with its own Level Zero loader (`ze_loader.dll`).

This zip is built with Intel's compiler (`icx-cl`) rather than Microsoft's, because only
Intel's compiler builds the SYCL backend, and it uses the C runtime as DLLs: the zip
carries them, Intel's SYCL runtime and the loader next to `vmaf.exe` and next to the
test programs, so nothing on your PC is used instead. Its CPU checks therefore measure
Intel's build of the CPU code, not MSVC's.

On top of everything the x64 zip runs, the SYCL zip, on every Intel GPU it finds (at
most four), one after the other:

- runs every CPU feature extractor on the four test videos with `--backend sycl` at
  full precision and compares each value with the CPU's;
- runs the project's parity gate for every SYCL twin on every fixture;
- runs the SYCL device tests of the build and the audit that no kernel uses scratch
  memory;
- says which open state rows your GPU's measurements close
  ([Intel GPU state rows](#intel-gpu-state-rows)).

No one has run the Windows SYCL build on a GPU before: GitHub's build machines have
none, and the project's Intel GPUs run Linux. Without an Intel GPU or its driver the
report says `gpu (sycl): no_device` and names Level Zero; that is not a failure.

### What is in the zip

Sizes are approximate; the exact file list with sizes is `bundle-files.txt` in the
workflow run that built it. The download is about 44 MB for x64, 39 MB for arm64 and
330 MB for the CUDA zip (measured on the zips of 2026-10-04; the SYCL zip had not been
built yet). Every file in it is Deflate-compressed by zopfli, which writes
smaller Deflate data than zlib's strongest level: Deflate is the strongest method that
`tar`, Explorer's "Extract All" and PowerShell's `Expand-Archive` all unpack on
Windows 10.

| Path | What |
| :--- | :--- |
| `run.cmd` | the one command (about 20 lines) |
| `runtime\` | a Python 3.13 interpreter ([python-build-standalone](https://github.com/astral-sh/python-build-standalone), pinned by SHA-256, standard library only, about 31 MB) with the two Microsoft Visual C++ runtime DLLs it needs |
| `tester\` | the report program, plain Python (`tools/rc1-tester/` in the repository) |
| `build\tools\vmaf.exe` | the VMAFx command line tool; libvmaf and the C runtime are linked in |
| `tests\` | unit test programs, each with libvmaf linked in (most of the zip's size); in the CUDA zip each carries the CUDA kernels, stored compressed (2.6 MB instead of 11 MB, [ADR-1590](../adr/1590-device-code-compression.md)) |
| `build\tools\*.dll`, `tests\*.dll` | SYCL zip only: the Microsoft Visual C++ runtime DLLs, Intel's SYCL runtime (`sycl8.dll`, the Unified Runtime loader and its Level Zero adapters, the compiler's math libraries, `umf.dll`, `libhwloc-15.dll`) and the Level Zero loader, next to the programs that load them |
| `python\test\resource\` | Netflix test videos, each checked against a pinned SHA-256 (about 57 MB) |
| `reference\`, `image\` | scores recorded by the build, manifests |
| `licenses\` | `THIRD_PARTY_NOTICES.txt` and the licence texts of everything above |

### Windows SmartScreen and Smart App Control

The programs carry no code signature, because the project has no code-signing
certificate. Check the zip as in step 2 (and, if you can, as in the next section)
before you run anything from it.

- **Downloaded with `curl.exe` and unpacked with `tar.exe`** (the steps above): the
  files carry no "downloaded from the internet" mark, and Windows does not ask.
- **Downloaded with a browser**: the browser may say the file is not commonly
  downloaded, and Windows marks the zip; files unpacked from it with Explorer carry
  the mark too, and Microsoft Defender SmartScreen may show "Windows protected your PC"
  when such a program starts. Check the zip first (step 2), then remove the mark from
  the zip before you unpack it: `Unblock-File vmafx-tester-windows-<ARCH>-<VERSION>.zip`.
  This removes only that mark, like clearing the quarantine flag on a Mac; it does not
  change any Windows setting. Do not click "Run anyway" for a zip you have not checked.
- **Smart App Control** (Windows 11; Windows Security, App & browser control) blocks
  programs without a signature whatever way they came. If it is on, the zip cannot run
  on that machine. We do not ask you to turn it off; use another machine, or the
  container image of [B](#b-container-image) if you have Docker.

### Check the download more closely (optional)

The checksum in step 2 comes from the same place as the zip, so it detects a broken
download, not a forged one. Two independent checks tie the zip to the repository's
hosted build. Either needs a tool you may not have; neither is needed to run the zip.
To also tie it to VMAFx's GitHub account and not only its name, add the owner-ID
check from [the signer's owner](../development/release.md#the-signers-owner-not-only-its-name)
to the `gh attestation verify` line.

```powershell
# GitHub build provenance (needs the GitHub CLI, gh):
gh attestation verify vmafx-tester-windows-<ARCH>-<VERSION>.zip -R VMAFx/vmafx

# Sigstore keyless signature (needs cosign; the .sigstore.json file is next to the zip):
cosign verify-blob --bundle vmafx-tester-windows-<ARCH>-<VERSION>.zip.sigstore.json `
  --certificate-identity-regexp '^https://github\.com/VMAFx/vmafx/\.github/workflows/windows-tester-bundle\.yml@refs/heads/master$' `
  --certificate-oidc-issuer https://token.actions.githubusercontent.com `
  vmafx-tester-windows-<ARCH>-<VERSION>.zip

# The SPDX software bill of materials (the .spdx.json asset) is attested on the zip:
gh attestation verify vmafx-tester-windows-<ARCH>-<VERSION>.zip -R VMAFx/vmafx `
  --predicate-type https://spdx.dev/Document/v2.3
```

To read what you are about to run: `run.cmd` is in the zip, the report program is
[`tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py`](https://github.com/VMAFx/vmafx/blob/master/tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py)
with its neighbours `hw_*.py` (the Windows host facts are `hw_winfacts.py`), and the
build is [`scripts/ci/build-windows-tester-bundle.py`](https://github.com/VMAFx/vmafx/blob/master/scripts/ci/build-windows-tester-bundle.py)
run by [`windows-tester-bundle.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/windows-tester-bundle.yml).

### Remove it afterwards

```powershell
cd ..; Remove-Item -Recurse -Force vmafx-tester-windows-<ARCH>-<VERSION>, vmafx-tester-windows-<ARCH>-<VERSION>.zip*
```

## Licences of what you download

Every package carries the licence of everything in it, and its publishing
workflow refuses to build a package with a file whose licence is not recorded
([ADR-1503](../adr/1503-tester-artifact-licensing.md)).

- **Where**: `licenses/THIRD_PARTY_NOTICES.txt` in the macOS bundle and the Windows
  zip, `/opt/vmafx/licenses/THIRD_PARTY_NOTICES.txt` in the container images. The file lists
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
- **AMD GPU image only**: the ROCm 10.1.0 runtime files in `/opt/vmafx/lib/rocm` are
  open source and ship unmodified: the HIP runtime, `rocprofiler-register` and `kpack`
  under MIT, the ROCm runtime under NCSA, the code object manager and the LLVM and Clang
  libraries it links under Apache-2.0 WITH LLVM-exception, and the system libraries ROCm
  bundles under their own licences (libdrm MIT, zlib, zstd BSD-3-Clause, liblzma 0BSD,
  bzip2). Their texts are in `/opt/vmafx/licenses/rocm/` and `/opt/vmafx/licenses/texts/`.
  Two of the bundled libraries are LGPL (elfutils' `libelf`, numactl's `libnuma`): you
  may replace them, and their corresponding source (the upstream archives and the
  TheRock tree that built them) is in `ghcr.io/vmafx/vmafx:<VERSION>-tester-hip-source`
  with the source of the image's Debian packages.
- **Windows zip only**: the C and C++ runtime is Microsoft's, linked into `vmaf.exe`
  and the test programs when they were built (`/MT`), so no runtime DLL is installed or
  shipped for them. The interpreter needs two Microsoft Visual C++ runtime DLLs
  (`runtime\vcruntime140.dll`, `vcruntime140_1.dll`); they are Microsoft Distributable
  Code, copied unmodified from the Visual Studio of GitHub's build machine. Both are
  governed by the licence terms of Visual Studio Enterprise 2026 (Last Updated
  October 1, 2025), whose text is `licenses\texts\visual-studio-2026-license-terms.txt`.
  The notices state the terms those terms ask a distributor to pass on (use with these
  programs only, pass them on only as part of the zip, no reverse engineering where the
  law does not allow it, provided as is), and that EUPL-1.2 covers only VMAFx files,
  never Microsoft's. Nothing in the zip is copyleft,
  so it has no source companion. The CUDA zip ships no NVIDIA file either: it uses your
  driver's `nvcuda.dll`; the NVIDIA code inside its GPU kernels comes with the CUDA
  Toolkit End User License Agreement (`licenses\nvidia\CUDA-EULA.txt`) and the notices
  of the nv-codec-headers loader (`licenses\nv-codec-headers\`), as in the NVIDIA GPU
  image.
- **Windows SYCL zip only**: its programs use the C runtime as DLLs (`/MD`, which
  Intel's SYCL compiler requires), so the zip carries the Microsoft Visual C++ runtime
  DLLs they import next to them, under the same Visual Studio terms. Intel's SYCL
  runtime files are Redistributables of the Intel End User License Agreement for
  Developer Tools, listed in the compiler's `credist.txt`; that agreement, its
  third-party notices and the list are in `licenses\intel\intel-oneapi-dpcpp-runtime\`.
  UMF (Apache-2.0 WITH LLVM-exception) and hwloc (BSD-3-Clause, built by Intel) carry
  their own texts, and the Level Zero loader, built from its source on GitHub's machine,
  is MIT-licensed. None of it is copyleft.
- **SBOM**: each package has an SPDX software bill of materials attested by the
  publishing workflow: the `.spdx.json` release asset for the macOS bundle and each
  Windows zip, an attestation on each platform manifest of the container, and an
  attestation on the digest of each GPU image. The container's SBOM is attested on the
  platform manifest the tag's index lists, so look that digest up first:

  ```sh
  docker buildx imagetools inspect --raw ghcr.io/vmafx/vmafx:<VERSION>-tester \
    | jq -r '.manifests[] | select(.platform.os == "linux" and .platform.architecture == "amd64") | .digest'
  gh attestation verify oci://ghcr.io/vmafx/vmafx@<that digest> -R VMAFx/vmafx \
    --predicate-type https://spdx.dev/Document/v2.3
  ```

  Use `arm64` for the arm64 manifest. The build provenance is attested on the index the
  tag points at (`docker buildx imagetools inspect ghcr.io/vmafx/vmafx:<VERSION>-tester
  --format '{{json .Manifest}}' | jq -r .digest`):

  ```sh
  gh attestation verify oci://ghcr.io/vmafx/vmafx@<index digest> -R VMAFx/vmafx
  ```

## What the report contains

One JSON document (schema: [`docs/hardware-reports/report.schema.json`](../hardware-reports/report.schema.json)):

- **Host facts**: CPU model string, the allow-listed `cpuinfo` / `sysctl` keys, CPU
  feature flags, `AT_HWCAP` / `AT_HWCAP2` on Linux, the dispatch flags the fork selects
  (NEON always on arm64, SVE2 only when the kernel reports it, as `core/src/arm/cpu.c`
  does), kernel release, macOS version and hardware model identifier (for example
  `Mac16,1`) and the Metal device name and family on a Mac. On Windows the processor's
  name, vendor and family come from the registry key where Windows describes it, the
  features from `IsProcessorFeaturePresent`, and the Windows version from Python's
  `platform` module.
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
  A test program that is killed by a signal, times out, or exits with a failure
  status without reporting a failing case is named in `unit_tests.reason` with what
  happened and where, for example
  `test_metal_ssimulacra2_parity: killed by signal 11 (SIGSEGV) during case
  test_ssimulacra2_rejects_monochrome`. The case it never finished is recorded as
  `fail` with the message `no verdict printed: ...`. The terminal summary prints the
  same text after the `unit tests:` line.
- **Errors of a vmaf run**: when `vmaf` fails on a fixture (dispatch, Metal or GPU
  equivalence), the fixture's `error` starts with `vmaf exited <status>` (and the
  signal's name for a crash) and the last line `vmaf` printed, followed by every
  `problem ...`, `error: ...` and libvmaf `ERROR` / `WARNING` line of its output,
  each once, at most 20 lines and 4 KB. The last line alone is often a warning
  printed while closing, after the message that names the failure.
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
- `run.cmd: this zip is for AMD64 Windows, this machine is ARM64` (or the other way
  round) — download the zip for your processor (see [F](#f-native-windows-zip-x64-or-arm64)).
- The Windows CUDA zip says `gpu (cuda): no_device` — Windows has no NVIDIA driver
  (`nvcuda.dll` is not in System32), or your GPU is older than the RTX 30 series; the
  reason says which.
- Windows says "Windows protected your PC", or Smart App Control blocked a program —
  see [Windows SmartScreen and Smart App Control](#windows-smartscreen-and-smart-app-control).
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
