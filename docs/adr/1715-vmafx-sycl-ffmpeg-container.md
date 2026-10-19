<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1715: `Containerfile.vmafx` — a self-contained SYCL + patched-ffmpeg image with a build-time golden gate

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `build`, `container`, `sycl`, `ffmpeg`, `oneapi`, `testing`, `fork-local`

## Context

The release images (`docker/Dockerfile.production`, `docker/Dockerfile.production-gpu`,
[ADR-1129](1129-release-container-runtime-alignment.md),
[ADR-1368](1368-oneapi-release-image-debian13.md)) ship the `vmaf` CLI and
library. None of them ships the fork's FFmpeg filters (`libvmaf_sycl`,
zero-copy QSV/VA import), and none runs the Netflix golden tests while it
builds. The dev container (`dev/Containerfile`) has both, but it is the large
all-backends image (CUDA, HIP, oneAPI, MCP). Testing the SYCL FFmpeg path on an
Intel GPU host needed a smaller image that builds from public inputs only.

## Decision

`Containerfile.vmafx` is a three-stage build:

1. **`runtime`** — `ubuntu:26.04` (digest-pinned) plus the Intel GPU compute
   stack (NEO Level Zero driver, OpenCL ICD, gmmlib, IGC, ocloc, Level Zero
   loader) at the versions pinned in `build-config.env`, installed by
   `scripts/ci/install-intel-ocloc.sh --components runtime` (the same script
   and pins as the oneAPI release image), plus VA-API / oneVPL runtime.
2. **`build`** — `runtime` + `intel-basekit` (icpx, lld) + GCC + meson /
   ninja + FFmpeg build dependencies + the Python golden-test stack. It builds
   libvmaf (SYCL only, AOT via the pinned ocloc for the option's default
   target list, Xe2 included since
   [ADR-1468](1468-sycl-sub-group-sizes-every-aot-target.md), toolchain per
   [ADR-1714](1714-hybrid-gcc-cpu-icpx-sycl.md)), then FFmpeg at `FFMPEG_TAG`
   (the tag `ffmpeg-patches/series.txt` is written against, checked out with
   `scripts/ci/checkout-annotated-tag.sh`) with every patch applied, then runs
   the CPU golden tests. A failing golden assertion fails the image build.
3. **`prod`** — `runtime` + the installed `vmaf` / `ffmpeg` binaries,
   `libvmaf.so`, headers and the `ldd`-curated oneAPI runtime libraries, not
   the ~10 GB `intel-basekit` tree.

GPU tests cannot run during `podman build` (no `/dev/dri`);
`scripts/test/run-all-tests.sh` runs the SYCL suites against the built image
with `--device /dev/dri`, and `scripts/test/reference_report.py` prints the
Netflix reference table (CPU and SYCL vs the Netflix values).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Separate three-stage SYCL + FFmpeg image (chosen)** | Small prod image; FFmpeg filters included; golden gate at build time; public inputs only | One more Containerfile to maintain | — |
| Add an FFmpeg target to `docker/Dockerfile.production-gpu` | One release Dockerfile | Release images are built per tag by CI with their own matrix and base (Debian 13); adding FFmpeg and a golden gate changes every release build | Larger blast radius |
| Use `dev/Containerfile` | Already has FFmpeg | Large all-backends image; too heavy for a GPU test host | Wrong size and scope |

## Consequences

- **Positive**: one command builds a tested SYCL + FFmpeg image on any Intel
  GPU host. Verified on the NAS (Xeon D-2143IT, Arc A380): the build-time
  golden gate passes (271 passed, 12 skipped); the 980 MB prod image scores
  the three Netflix pairs within `places=4` on CPU and SYCL, SYCL equals CPU
  bit for bit on `integer_adm2`, `integer_adm3`, `integer_vif_scale0`,
  `integer_motion2` and VMAF (src01), the `libvmaf`, `libvmaf_sycl` and
  `libvmaf_tune` FFmpeg filters are present, and libvmaf's math binds to glibc
  `libm` (research digest). That run built AOT without the Xe2 targets,
  which master could not compile then (`T-SYCL-XE2-SUBGROUP8-AOT-2026-10-02`,
  closed: fixed on master by #1842 before this branch merged). With
  `libmfx-gen1.2` QSV works in the image, and `libvmaf_sycl` zero-copy (QSV
  decode) scores 8-bit and 10-bit HEVC bit for bit like the CPU path, no NaN.
- **Negative**: the first build installs oneAPI and builds FFmpeg (cached
  afterwards).
- **Neutral / follow-ups**: keep `FFMPEG_TAG` equal to the tag in
  `ffmpeg-patches/series.txt` (rule 14). `xxd` stays required in the build
  stage (it embeds the built-in models; without it the default-model path and
  the matching golden test break). meson installs to the multiarch libdir
  (`lib/x86_64-linux-gnu`), which the `prod` stage copies from.

## Supply-chain impact

- **New dependencies**: none beyond what the oneAPI release image and
  `dev/Containerfile` already pull: `ubuntu:26.04`, Intel oneAPI
  `intel-basekit` (apt), the Intel GPU stack pinned in `build-config.env`
  (SHA-256 checked), FFmpeg at `FFMPEG_TAG`.
- **Build-time fetches**: Intel apt repo, GitHub releases of the Intel GPU
  stack, the FFmpeg git tag, PyPI (golden-test requirements).

## References

- 2026-10-01 popup: port `Containerfile.vmafx` and the test harness onto the
  new branch from master.
- Research digest: [`docs/research/1715-vmafx-sycl-ffmpeg-container.md`](../research/1715-vmafx-sycl-ffmpeg-container.md).
- [ADR-1714](1714-hybrid-gcc-cpu-icpx-sycl.md), [ADR-1368](1368-oneapi-release-image-debian13.md),
  [ADR-1360](1360-sycl-aot-compile-time-device-codegen.md), [ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md).
- Originally drafted as ADR-1441; renumbered to 1503 on the rebase onto
  master `b01ffe42d`, where 1441 had been taken, and to 1562 on the
  rebase onto master `2889f963a`, where 1503 had been taken; then to 1594 on the
  rebase onto master `f224b1b42`, where 1562 had been claimed by other branches
  (and 1562 taken by master).
- Renumbered to 1715 when it was ported to `VMAFx/vmafx` master `dc9cd9481`,
  where 1594 had been taken and numbers up to 1713 had been claimed by
  `VMAFx/vmafx` branches.
