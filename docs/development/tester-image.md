<!-- markdownlint-disable MD013 MD024 -->
# Tester image and macOS bundle: maintainer notes

What the tester packages are, how they are built and published, what to do by hand,
and how reports reach the tree. The tester-facing steps are in
[the tester guide](../usage/tester-image.md); the decisions are
[ADR-1492](../adr/1492-tester-image-arm64-report.md),
[ADR-1493](../adr/1493-macos-tester-bundle.md) and, for the GPU images,
[ADR-1505](../adr/1505-intel-gpu-tester-image.md) (Intel),
[ADR-1509](../adr/1509-nvidia-gpu-tester-image.md) (NVIDIA) and
[ADR-1511](../adr/1511-amd-gpu-tester-image.md) (AMD).

## Pieces

| Piece | Where |
| :--- | :--- |
| Container image | [`docker/Dockerfile.tester`](https://github.com/VMAFx/vmafx/blob/master/docker/Dockerfile.tester), build inputs in `tools/rc1-tester/image/` |
| Intel GPU image | target `final-sycl` of the same Dockerfile; `image/sycl-tests.txt` (device tests), `sycl-rows.json` (state rows), `sycl-runtime.json` (the Intel runtime files it ships); its licence record is the `sycl-image` artifact of `licensing.json` |
| AMD GPU image | target `final-hip` of the same Dockerfile; `image/hip-tests.txt` (device tests), `hip-rows.json` (state rows), `hip-runtime.json` (the ROCm runtime files it ships); `image/hip-targets.json` is written by the build from its offload targets; its licence record is the `hip-image` artifact of `licensing.json` |
| NVIDIA GPU image | target `final-cuda` of the same Dockerfile; `image/cuda-tests.txt` (device tests), `cuda-rows.json` (state rows); `image/cuda-targets.json` is written by the build from its gencode list; its licence record is the `cuda-image` artifact of `licensing.json` |
| macOS bundle | `scripts/ci/build-macos-tester-bundle.sh`, `tools/rc1-tester/image/macos/` |
| Report program | `tools/rc1-tester/src/vmaf_rc1_tester/hw_*.py`, launcher `tools/rc1-tester/vmaf-tester-report`; the GPU section is `hw_gpu.py`, its SYCL backend `hw_sycl.py` and `hw_l0probe.py`, its CUDA backend `hw_cuda.py` and `hw_cudaprobe.py`, its HIP backend `hw_hip.py` and `hw_hipprobe.py` |
| Report schema and gate | `docs/hardware-reports/report.schema.json`, `scripts/ci/check-hardware-reports.py` (in `make docs-fragments-check`) |
| Index page | `scripts/docs/generate-hardware-reports.py --write` (in `make docs-fragments-write`) |
| Workflows | `.github/workflows/docker-publish-tester.yml`, `.github/workflows/macos-tester-bundle.yml` |
| Licence record, notices and gate | `tools/rc1-tester/image/licensing.json`, `tools/rc1-tester/image/licensing.py` ([ADR-1503](../adr/1503-tester-artifact-licensing.md)) |

One implementation serves both packages: the macOS bundle ships the same Python report
code, under a bundled interpreter, and the same schema and gate validate both reports
(HISS-19).

## Publishing, by hand

Nothing publishes on merge except a build-and-test run of the image on pushes to master.

1. **Container**: dispatch `Publish Tester Image` on `master` with exactly one of `ref`
   (a commit SHA reachable from master, or `master`, resolved to its SHA) or `tag` (a
   published release tag). The source is that commit, built with master's recipe as
   ADR-1347 does for recovery; anything not reachable from master is refused. The image
   tag is `<git describe of the commit>-tester`, for example
   `v1.0.0-rc.2-312-g1a2b3c4d-tester`. Publishing needs approval in the `tester-publish`
   environment (master only); `release-publish` is for product releases and is not used. The workflow builds both architectures on native runners, runs the
   documented `docker run` line on each, validates the report, pushes by digest, merges
   one index, signs it keyless and attests it, and publishes
   `ghcr.io/vmafx/vmafx:<tag>-tester`. The package `ghcr.io/vmafx/vmafx` is already public,
   so nothing needs a click; check once with a logged-out `docker pull`.
2. **macOS bundle**: dispatch `Publish macOS Tester Bundle` on `master` with `ref` (or
   `tag`) and `publish: true`; the file is `vmafx-tester-macos-arm64-<git describe>`. `publish: false` builds, runs the bundle's own report on the hosted
   runner and uploads a 14-day workflow artifact only. With `publish: true` the
   `tester-publish` environment gate (master only, maintainer approval) applies, then the bundle is attested, signed and
   attached to a new prerelease `tester-<date>-<sha8>` (not a product release). The
   `<git describe>` is `git describe --tags --match 'v*.*.*'` of the source commit
   (for example `v1.0.0-rc.2-311-g2414774ea`; a commit that carries the tag gives the
   bare tag): the `tester-*` tags of earlier prereleases must never supply the version.
   The release step, and only that step, uses the release-bot identity of
   `release-please.yml` (the App token when `RELEASE_BOT_APP_ID` and
   `RELEASE_BOT_PRIVATE_KEY` exist, else `RELEASE_BOT_TOKEN`, else the step fails and
   names the missing secrets; it prints which one it used). The job token cannot create
   the tag of a commit that is behind master and differs in `.github/workflows/`: the
   token cannot hold the `workflows` permission and GitHub answers `HTTP 403: Resource not
   accessible by integration`. A release made by the release-bot identity starts other
   workflows, as release-please's own pushes do.

3. **GPU images**: the same workflow also builds `final-sycl`, `final-cuda` and
   `final-hip` (job `build-gpu`, one matrix leg per kit, linux/amd64 only) and runs the
   documented command without a GPU (the report must say `gpu.status` `no_device` with
   the missing option, `--device /dev/dri`, `--gpus all` or `--device /dev/kfd`, every
   CPU check passing); the dispatch publishes `ghcr.io/vmafx/vmafx:<describe>-tester-sycl`,
   `-tester-cuda` and `-tester-hip` in job `publish-gpu`: signed, with provenance and an attested SPDX SBOM (syft), each with its
   source image `<describe>-tester-<kit>-source` (target `<kit>-source-export`). Like the
   CPU image, each build cannot finish without its licence check (stage
   `<kit>-licence-check`, artifacts `sycl-image`, `cuda-image` and `hip-image`, see
   [Licensing](#licensing)). The hosted runner has no GPU: every device measurement
   happens on the tester's machine. Before giving a tag to a tester, run it on the
   project's Arc A380, RTX 4090 or gfx1036 (see [Local checks](#local-checks)).

Give the tester `<TESTER-TAG>` and `<VERSION>` (the `git describe` string) from the run summary.

## Licensing

Every published tester package follows [ADR-1503](../adr/1503-tester-artifact-licensing.md);
the kits for other hardware follow the same rules. What the two packages do:

- **Notices inside.** `licensing.py notices` writes `THIRD_PARTY_NOTICES.txt` and the
  licence texts into `/opt/vmafx/licenses/` (container, a `RUN` in the `runtime` stage with
  the repository inputs bind-mounted) and `licenses/` (bundle, before the report run). The
  VMAFx section is computed, not listed: `licensing.py scan-build` reads `ninja -t deps`
  of the build and the SPDX header (or `REUSE.toml` entry) of every repository file the
  build compiled, and writes `vmafx-compiled-sources.json`, which ships next to the notices.
- **The gate.** `licensing.py check` walks the finished tree and fails on any file that no
  component of `licensing.json` claims, a Debian package without its copyright file, a
  dist-info without a licence file, a library grafted into a wheel that is unrecorded,
  modified (compared with the wheel's `RECORD`) or copyleft without a recorded source
  archive, a compiled or repository file whose licence the component does not allow, an
  interpreter version without its recorded `Doc/license.rst`, or notices that do not list
  every component, package and dist-info. In the container it is the `licence-check`
  stage, which the `final` stage depends on through its receipt
  (`/opt/vmafx/licence-check.json`); in the bundle it runs right before `pack`.
- **Source.** The container's copyleft parts have their source in
  `ghcr.io/vmafx/vmafx:<tag>-tester-source`, the Dockerfile's `source-export` target, built
  and pushed per architecture by the publishing run and merged into one signed index:
  `licensing.py sources` lists the Debian source packages of every installed package at its
  version (with `Built-Using` and `Static-Built-Using`) and the recorded source RPMs of the
  grafted GCC runtime libraries (matched by ELF build ID); `fetch-sources` downloads them
  (`apt-get source`, falling back to snapshot.debian.org) and writes `SOURCES.txt`. The
  bundle has no copyleft object code.
- **SBOM.** Syft v1.51.1 writes an SPDX JSON SBOM of each pushed platform image (attested on
  its digest with `actions/attest`) and of the unpacked bundle (attested on the archive,
  published as the `.spdx.json` asset).

The Intel GPU image is the artifact `sycl-image`. Two kinds of record exist for it: the
Intel SYCL runtime files are `fixed` components whose texts are the compiler's `LICENSE`,
`third-party-programs.txt` and `credist.txt` (and UMF's and TCM's), copied next to the files
by `prepare_build.py intel-runtime`, with the terms the Intel licence asks a distributor to
pass on as notes; the Intel GPU stack (compute runtime, IGC, gmmlib, Level Zero loader)
comes from the pinned GitHub releases as Debian packages, so the component of kind
`dpkg-foreign` names those packages: they need no `/usr/share/doc/<package>/copyright`
(IGC and the loader ship none; their texts are `fetched_texts`, pinned by URL and SHA-256)
and no Debian source package, and the component names their source instead.

The NVIDIA GPU image is the artifact `cuda-image` and ships no NVIDIA file: `libvmaf`
loads the host driver's `libcuda.so.1` at run time (the NVIDIA Container Toolkit lends
it), and the `cuda-runtime` stage fails when a file named like an NVIDIA library appears;
the build stage fails when a binary's `NEEDED` names one. The NVIDIA content is inside the
VMAFx binaries: the kernels' device code holds CUDA header code and the `libdevice`
library nvcc links in, which Attachment A of the CUDA Toolkit EULA lists as
distributable. Its component `nvidia-cuda-device-code` has no paths; its text is the EULA,
copied in the build stage from the copyright file of the package that installs
`libdevice.10.bc` (the build checks the text's date), and its notes pass on the EULA's
terms. The component `nv-codec-headers` carries the MIT notices of the CUDA loader headers,
copied from them at build time. The scan records each kernel object
(`src/<kernel>.fatbin.c`, the `bin2c` output) with the licence of the `.cu` source it was
compiled from (`generated_build_files` rule with `compiled_from`).

The AMD GPU image is the artifact `hip-image`. Its build stage streams `/opt/rocm` out
of the pinned ROCm image (`scripts/ci/install-rocm-from-image.sh --keep-docs`, which keeps
`share/doc` for the licence texts) and `prepare_build.py rocm-runtime` copies the files of
`hip-runtime.json` unmodified into `/opt/vmafx/lib/rocm`, keeping `llvm/lib` and
`rocm_sysdeps/lib` next to them as the libraries' RPATHs expect. One `fixed` component per
ROCm part carries the text ROCm installs in `share/doc` or a `fetched_texts` entry where
it installs none (kpack, LLVM, Clang and the bundled system libraries, pinned by URL and
SHA-256). The bundled system libraries are component `rocm-sysdeps` with
`vendored_libraries`: one rule per library, and for the two LGPL ones (`libelf`,
`libnuma`) the ELF build IDs of the ROCm 10.0.0 files mapped to their source archives
(the upstream tarballs TheRock pins and the TheRock tree with its patches); the check
fails on a rule that matches no file or a copyleft library of another build, and
`licensing.py sources` puts the archives into the `-tester-hip-source` image. A ROCm bump
changes those build IDs: read the new TheRock manifest, record the new archives and IDs.

When the check fails after a lock, base-image or interpreter bump, record what changed in
`licensing.json`: a new grafted library needs its licence and, when copyleft, the source
package its build ID comes from in `source_archives` (URL and SHA-256); a new interpreter
version needs its `Doc/license.rst` in `cpython_license_rst`; a new licence in compiled
code needs its text in `LICENSES/` and in `spdx_texts`; a new kind of file needs a
component. Never strip or patch a vendor binary to make room.

## The GPU section, and how a backend plugs in

Schema 3 adds one `gpu` section that every GPU package fills the same way
(`hw_gpu.py`): per device the twins against the CPU of the same image (per fixture and
per parity-gate feature, judged at the gate's bound for that twin from
`image/gpu-twins.json`, which the build writes from the staged gate), the gate's cells
of the backend held exact, the device tests of `image/gpu-tests.json`, the backend's
audits, and the state rows of the backend's row map for that device's family. A new
backend (CUDA, HIP) adds a module like `hw_sycl.py` that builds a `GpuBackend`: a
`discover` function returning how the container reaches the device (`access`, with a
`path`), the runtime versions and the devices (each with an `index` and allow-listed
`facts`, among them a `family` the row map can name, never a UUID or bus address), a
`device_env` that pins a run to one device (`hw_cuda.py`: `CUDA_VISIBLE_DEVICES` under
`CUDA_DEVICE_ORDER=PCI_BUS_ID`, the order its probe lists the devices in), the
parsers of any audit test's output, and the name of its row map; it registers it in
`GPU_BACKENDS` of `hw_report.py`, and its image sets `VMAFX_GPU_BACKEND` and stages the
twin bounds (`prepare_build.py twins`), the device tests (`prepare_build.py stage ...
gpu-tests.json`, a list of tests or `suite:` lines) and the gate. The schema, the CI
gate and the verdict need no change; `tools/rc1-tester/tests/test_hw_gpu.py` shows the
contract with a fake backend. The Metal sections of the macOS bundle predate this and
keep their schema-2 form.

## What the hosted macOS runner cannot show

The hosted runner is a virtual machine. The workflow runs the bundle's report there and
records which Metal twins ran. A runner without a usable Metal device makes `vmaf` exit
100 for `--backend metal`; the report records `no_device` (not exercised, not a
failure) and the Metal parity unit tests exit 77 (skipped). Whatever the runner cannot
exercise is first proven on the tester's machine: the Metal twins on a real GPU, the
bundle under Gatekeeper and `sandbox-exec` on his macOS version, the ad-hoc signature
on his hardware, and the interpreter on his system libraries.

## Report intake

A report PR adds one file `docs/hardware-reports/<date>-<cpu-slug>.json`. CI runs
`scripts/ci/check-hardware-reports.py` (schema, file name, integrity hash, hosted-build
facts, verdict consistency, host key allow-list). The index is not checked, so an outside
contributor need not run a generator: after merging, run `make docs-fragments-write`
and commit `docs/hardware-reports/index.md`. An outside contributor's pull request meets
the PR template's deliverables checklist; complete it on his behalf or push a commit
to his branch. For an issue submission, commit the attached file yourself with
`Co-authored-by: Name <address>` when the form gives both. No tester name belongs in
tracked files other than a commit trailer the person asked for.

## Updating the pins

| Pin | Where | Rule |
| :--- | :--- | :--- |
| Base images | `build-config.env` (`RELEASE_BUILDER_BASE`, `RELEASE_PYTHON_BASE`), mirrored in `docker/Dockerfile.tester` | `scripts/ci/check-base-image-single-source.sh --write` |
| Python test stack | `python/requirements-test-lock.txt` | `make python-locks-write` |
| Fixtures | `tools/rc1-tester/image/fixtures.sha256`, `VMAF_RESOURCE_COMMIT` | change both together; the build checks every SHA-256 |
| macOS interpreter | `PBS_URL`, `PBS_SHA256`, `PBS_FULL_URL`, `PBS_FULL_SHA256` in `macos-tester-bundle.yml` | the `install_only_stripped` archive and the `pgo+lto-full` archive of the same release (its licence texts); take both hashes from the release's `SHA256SUMS` |
| Licence record | `tools/rc1-tester/image/licensing.json` | change with the package contents; the build fails until it matches |
| Report validation | `requirements/locks/jsonschema.txt` | universal lock for Python 3.12 and later (`--universal --python-version 3.12`): the hosted runners differ (3.12 on `ubuntu-latest`, 3.14 elsewhere) and `referencing` needs `typing-extensions` below 3.13 |
| Unit tests | `tools/rc1-tester/image/unit-tests.txt`, `unit-tests-macos.txt` | a name absent from a build is skipped; fewer than ten found fails the build |
| Intel GPU runtime | `INTEL_NEO_VERSION`, `LEVEL_ZERO_VERSION`, `ONEAPI_*` in `build-config.env`; `tools/rc1-tester/image/sycl-runtime.json`; `fetched_texts` and the `intel-gpu-stack` component of `licensing.json` | a moved compute runtime or loader version fails the build until the licence text of the new version is recorded in `fetched_texts` (URL and SHA-256) and named by the component; a runtime file must stay in the compiler's `credist.txt` |
| Intel GPU state rows | `tools/rc1-tester/image/sycl-rows.json` | `tools/rc1-tester/tests/test_gpu_rows_contract.py` holds it to `docs/state.md`, the `gpu` suite and the gate |
| CUDA toolkit | `CUDA_VERSION` and the `CUDA_APT_*` versions in `build-config.env` (NVIDIA's `debian13` repository, `scripts/ci/install-cuda-toolkit.sh`); `NV_CODEC_HEADERS_COMMIT` in `docker/Dockerfile.tester` | a new CUDA version brings a new EULA: the build checks the EULA's "Last updated" date, so update that check, the `nvidia-cuda-device-code` component and ADR-1509's citation together after reading the new Attachment A |
| NVIDIA GPU state rows | `tools/rc1-tester/image/cuda-rows.json` | the same contract test; every CUDA family has a row, and each row holds every gate feature |
| ROCm runtime | `ROCM_BUILDER` in `build-config.env` (mirrored in `docker/Dockerfile.tester`); `HIP_GFX_TARGETS` in `docker/Dockerfile.tester`; `tools/rc1-tester/image/hip-runtime.json`; the `rocm-*` components, `fetched_texts` and `source_archives` of `licensing.json` | a new ROCm brings new file names, a new TheRock commit and new build IDs of the bundled LGPL libraries: the build and the licence check fail until `hip-runtime.json` and the record match; a new gfx target needs a row family (`tests/test_gpu_rows_contract.py`) |
| AMD GPU state rows | `tools/rc1-tester/image/hip-rows.json` | the same contract test; every family of `HIP_GFX_TARGETS` has a row, and each row holds every gate feature |

The Debian archive packages of the build stage are not version-pinned, as in the release
build (ADR-1346); the base image digest is.

## Shell scripts that run on macOS

The hosted macOS runner's `bash` is Apple's 3.2, and `run.sh` runs on the tester's Mac as POSIX `sh`.
No `mapfile`, `declare -A`, `${x,,}`, `|&`, `[[ -v ]]`, `coproc` or `local -n` in anything they run.
`tools/rc1-tester/tests/test_bash32_compat.py` scans for these, runs `run.sh` through
`shellcheck --shell=sh`, and (with Docker and `docker pull bash:3.2`) runs the build script and
the link check under a real bash 3.2 with stub tools.

## Local checks

```sh
python3 -m pytest -q tools/rc1-tester/tests          # report code, schema gate, bundle scripts
actionlint .github/workflows/docker-publish-tester.yml .github/workflows/macos-tester-bundle.yml
shellcheck tools/rc1-tester/image/macos/run.sh scripts/ci/check-macos-bundle-links.sh \
  scripts/ci/build-macos-tester-bundle.sh
docker build -f docker/Dockerfile.tester -t vmafx-tester:dev .
docker run --rm --network none --read-only --cap-drop ALL --security-opt no-new-privileges \
  --tmpfs /tmp vmafx-tester:dev > report.json
```

The Intel GPU image, on the project's Arc A380 (the device lock keeps other lanes off
the card; a run takes about 90 seconds):

```sh
docker build -f docker/Dockerfile.tester --target final-sycl --build-arg VMAF_BUILD_JOBS=8 \
  -t vmafx-tester:sycl-dev .
flock ~/.cache/vmafx-locks/sycl-a380.lock timeout 300 \
  docker run --rm --network none --read-only --cap-drop ALL --security-opt no-new-privileges \
  --tmpfs /tmp --device /dev/dri $(stat -c '--group-add %g' /dev/dri/renderD* | sort -u) \
  vmafx-tester:sycl-dev > report.json
```

The NVIDIA GPU image, on the project's RTX 4090 (about two and a half minutes):

```sh
docker build -f docker/Dockerfile.tester --target final-cuda --build-arg VMAF_BUILD_JOBS=6 \
  -t vmafx-tester:cuda-dev .
flock ~/.cache/vmafx-locks/cuda-4090.lock timeout 300 \
  docker run --rm --network none --read-only --cap-drop ALL --security-opt no-new-privileges \
  --tmpfs /tmp --gpus all vmafx-tester:cuda-dev > report.json
```

`timeout` stops the `docker` client, not the container: give the run a `--name` and
remove it after the timeout (`docker rm -f <name>`) inside the same locked command.

The AMD GPU image, on the project's gfx1036 (the ROCm runtime download is about 8 GB on
the first build):

```sh
docker build -f docker/Dockerfile.tester --target final-hip --build-arg VMAF_BUILD_JOBS=6 \
  -t vmafx-tester:hip-dev .
flock ~/.cache/vmafx-locks/hip-gfx1036.lock timeout 300 \
  docker run --rm --network none --read-only --cap-drop ALL --security-opt no-new-privileges \
  --tmpfs /tmp --device /dev/kfd --device /dev/dri \
  $(stat -c '--group-add %g' /dev/kfd /dev/dri/renderD* | sort -u) vmafx-tester:hip-dev > report.json
```

The gfx1036 now and then drops a stream's dispatches
(`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`): run a failing report again before
blaming the image.

An image built this way has `built_by_workflow: false` and the CI report gate refuses it,
as intended. The local build runs the licence check as well; to see the source companion,
build `--target source-export --output type=local,dest=<dir>` (about 670 MB of downloads). The x86_64 cross-architecture reference comes from the amd64 build
(`--target refs-export`); without it the `cross_arch_x86_scalar` section reads `missing`
and is informational.
