<!-- markdownlint-disable MD013 MD060 -->
# Research-1346: Building native release artifacts on a hosted runner inside the build-deps stage

- **Status**: Active
- **Workstream**: [ADR-1346](../adr/1346-hosted-slim-container-release-build.md), supersedes [ADR-1178](../adr/1178-dev-container-image-publish.md)
- **Last updated**: 2026-09-27

## Question

Can `build-artifacts` in `.github/workflows/supply-chain.yml` build the
native Linux bundle on a GitHub-hosted runner inside an image made from
`dev/Containerfile`, reliably within one job? Which stage, which image-build
mechanism, cache policy and timeout should it use? What does a
container-built bundle require from the systems that run it? And how can
every pull request rehearse that release build?

## Sources

- `gh run list` / `gh run view --json jobs` for
  `.github/workflows/dev-container-build.yml` (the ADR-0819 PR gate) and
  `.github/workflows/dev-container-publish.yml`, queried 2026-09-27.
- `gh run view --log` for publisher runs 35868459269 (success),
  36273853106, 35861745414, 35831873445, 35817279976, 35798423831 and
  35450966064 (cancelled).
- GitHub Actions dependency caching reference:
  <https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching>.
- `dev/Containerfile` (stage graph, the `build-deps` package list, the
  `gpu-sdks` / `libvmaf-build` downloads) and `build-config.env` (TWO TRACKS).
- Meson 1.10.1 as installed in `build-deps`:
  `mesonbuild/compilers/detect.py` (`defaults['gcc_static_linker'] = ['gcc-ar']`,
  tried before plain `ar` for GCC).
- Local runs on the maintainer workstation (Docker 29.8.1, buildx 0.37.1)
  of `scripts/ci/build-dev-container-stage.sh` and
  `scripts/release/build-native-release-artifacts.sh`, described below.

## Findings

### Which stage

`libvmaf-build` is the ~29.5 GB stage. It adds the GPU SDKs: `gpu-sdks`
downloads the Level Zero `.deb`s from GitHub releases and clones `vpl-gpu-rt`
by tag, and `libvmaf-build` itself fetches the ONNX Runtime tarball and
`nv-codec-headers` and clones SVT-AV1, VVenC and FFmpeg by tag, none of them
checked against a hash. `build-deps` is the first stage: the digest-pinned
`ubuntu:26.04` base (`DEV_BASE`), Ubuntu archive packages (gcc-13, clang-19,
Meson, Ninja, NASM, Python 3.14, git, xxd, jq), the
`/etc/vmafx-dev-container` marker and the `vmaf` user. It fetches nothing from
third parties and mounts no BuildKit secret. The maintainer chose it (ADR-1346
Q1).

Every tool the release path needs is in `build-deps`: `readelf`, `objdump`,
`ldd`, `realpath`, `sha256sum`, `git`, `python3`, `tar`, `gzip`, `xxd`,
`meson`, `ninja`, `gcc`, `nasm`, `pkg-config`, `jq`. There is no `cc`;
Meson finds `gcc` (GCC 13.4.0) through the stage's `update-alternatives`.
`libvmaf-build` later installs Ubuntu's default `gcc` package, whose
`/usr/bin/gcc -> gcc-15` link replaces the alternative, so that stage and the
developer image compile with GCC 15.2.0.

### Build time

| Job | Mechanism | Duration | Outcome |
|---|---|---|---|
| `Dev Container Build work` (PR gate), the 14 runs that did work between 2026-09-25 and 2026-09-26 | `docker build --target libvmaf-build`, default builder, no cache, no push | build step 28.0 / 34.5 / 35.1 min (min / median / max) | all succeeded |
| `dev-container-publish.yml`, last 10 runs | `docker/build-push-action`, `cache-from`/`cache-to: type=gha,mode=max`, push | successful build-and-push steps 50.5, 56.6 and 58.9 min | 3 succeeded, 6 cancelled at the 60-minute limit, 1 failed after 5 minutes |
| `build-deps`, workstation, `docker build --no-cache --target build-deps`, two runs | default builder | 113 s and 81 s; the apt step took 90 s and 61 s (317 MB fetched in 42 s and 21 s) | succeeded |
| `build-deps` via `build-dev-container-stage.sh build-deps` after an earlier `libvmaf-build` build on the same daemon | default builder | 5 s, every step `CACHED` | succeeded |

The last row is the rehearsal's situation in the PR gate. The resulting
`build-deps` image's eight layers are exactly the first eight of the 53 layers
of the `libvmaf-build` image built from the same tree, so the second build
reuses the first build's layers rather than rebuilding them.

The publisher's cache story is unchanged from the first version of this
digest: its successful run 35868459269 spent about 32 minutes in the BuildKit
solve, 499 s compressing layers, 47 s pushing, and ten minutes exporting a
`gha` cache that a 10 GB repository budget cannot hold for a ~29.5 GB layer
set. All six cancelled runs had pushed the image before being cancelled 16 to
18 minutes into the cache export, so cosign never ran.

### Cache scope and pinning

GitHub documents that a run can restore caches from its current ref and from
the default branch, so a tag-triggered release could only read caches that the
tag or `master` wrote. The release still uses no external cache:
`build-dev-container-stage.sh` passes no `--cache-from` / `--cache-to` and no
registry is involved. A fresh hosted runner's daemon cache is empty, so the
release builds `build-deps` from its pinned base.

That base is the only digest pin in `build-deps`. Its `apt-get install`
resolves against the Ubuntu archive at image-build time, so rebuilding an old
tag later may install newer compilers or Meson than the original release. The
release compile runs under `docker run --network none`, and no Meson wrap or
subproject download appears in its log.

### LTO archives in `build-deps`

The first in-container release build in `build-deps` failed at link time:
`test/test_context`, `test/test_pool_percentile`, `test/test_picture` and
`test/test_picture_v2` reported `undefined reference to 'vmaf_init'`,
`'vmaf_picture_unref'` and similar while linking against `src/libvmaf.a` with
`-flto=4`. Meson tries `gcc-ar` before `ar` for GCC. `build-deps` installs
`gcc-13` but not the default `gcc` package, so it had only `gcc-ar-13`, no
unversioned `gcc-ar`, and no `/usr/lib/bfd-plugins/liblto_plugin.so` (only
LLVM's `LLVMgold-19.so`). Meson therefore archived with plain `ar`, which
without the GCC plugin cannot index LTO objects. `libvmaf-build` never showed
this because its default GCC 15 package ships both.

Registering `gcc-ar`, `gcc-nm` and `gcc-ranlib` as `update-alternatives`
slaves of the `gcc` alternative fixes it: `meson-log.txt` then reads
``Detecting archiver via: `gcc-ar --version` -> 0`` and the full default target
set links. Installing the default `gcc` and `g++` packages on top of that
stage, as `libvmaf-build`'s ancestry does, still succeeds and leaves
`/usr/bin/gcc-ar -> gcc-ar-15`, so `libvmaf-build` is unchanged.

### Release build as an unnamed UID

The release job runs the image as the runner's UID and GID. Only UIDs 1000
(`ubuntu`, from the base image) and 2000 (`vmaf`, added by the stage) have a
passwd entry in the image; a process with any other UID gets `HOME=/`. The
proof below used UID 1001 to reproduce that case. Meson wraps the compiler in `ccache` because `build-deps` installs
it (`C compiler for the host machine: /usr/bin/ccache gcc`), and
`ccache gcc -c` as UID 1001 fails with `ccache: error: Permission denied`
because it cannot create `/.cache/ccache`. With `CCACHE_DISABLE=1` the same
command succeeds, so the release script's `CCACHE_DISABLE=1` is load-bearing.

`-Denable_dnn=disabled` is a no-op in `build-deps`: Meson reports
`Run-time dependency libonnxruntime found: NO` and `onnxruntime found: NO`.
It stays explicit so a stage that does ship ONNX Runtime (as `libvmaf-build`
does, where `auto` made `libvmaf.so` NEED `libonnxruntime.so.1`) cannot change
the bundle.

### End-to-end proof on a scratch release-candidate tag

A scratch bare repository received this branch's commit and a local-only
lightweight tag `v1.0.0-rc.1` (never in the real repository, never pushed).
A scratch clone was fetched the way `actions/checkout` fetches an unqualified
tag `ref`: `git fetch --no-tags --depth=1 origin
'+refs/heads/v1.0.0-rc.1*:refs/remotes/origin/v1.0.0-rc.1*'
'+refs/tags/v1.0.0-rc.1*:refs/tags/v1.0.0-rc.1*'`, then
`git checkout --force refs/tags/v1.0.0-rc.1`, and chowned to UID/GID 1001.
The stage was built with `bash scripts/ci/build-dev-container-stage.sh
build-deps "vmafx-release-build:${GITHUB_SHA}"`, and the release step ran as
the workflow runs it, capped at four CPUs like a hosted runner:

```bash
docker run --rm --pull never --network none --cpuset-cpus 0-3 \
  --user 1001:1001 --env GITHUB_SHA --volume "$PWD:/src" --workdir /src \
  "vmafx-release-build:${GITHUB_SHA}" \
  bash scripts/release/build-native-release-artifacts.sh 1.0.0-rc.1
```

It finished in 148 s and ended with `Verified Linux release runtime
1.0.0-rc.1 (git describe exactly on the release tag: v1.0.0-rc.1-0-g<commit>)
with materialized libvmaf.so.3 chain.` `meson-log.txt` shows
``Detecting archiver via: `gcc-ar --version` -> 0``, and
`scripts/ci/check-container-build.sh --verify artifacts` on the host accepted
the stamp, whose `git_commit` is the tagged commit. The same run with
`GITHUB_SHA` set to another commit stopped after the container assertion with
`ERROR: checked-out HEAD <commit> is not GITHUB_SHA <other>` and compiled
nothing. The staged tree held `libvmaf.so`,
`libvmaf.so.3`, `libvmaf.so.3.0.0` (identical bytes), `vmaf`,
`models.tar.gz` and a stamp with `image_title=vmaf-dev-mcp`.
`objdump -T` shows the newest glibc symbol of `libvmaf.so` is
`GLIBC_2.43` (`sqrtf`) and its newest libstdc++ symbol `GLIBCXX_3.4.30`; the
CLI's are `GLIBC_2.38` and `GLIBCXX_3.4.20`. `libvmaf.so` NEEDs only
`libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1` and `libc.so.6`; the CLI's
RUNPATH is `$ORIGIN/../src`.

### Where the bundle loads

`vmaf --version` with `LD_LIBRARY_PATH` pointing at the staged files, each
image digest-pinned:

| Image | glibc | Result |
|---|---|---|
| `ubuntu:26.04` (`DEV_BASE`) | 2.43 | `v1.0.0-rc.1-0-g<commit>` |
| `ubuntu:24.04` (today's `ubuntu-latest`) | 2.39 | `version 'GLIBC_2.43' not found (required by /a/libvmaf.so.3)` |
| `debian:13-slim` (`RELEASE_BUILDER_BASE`) | 2.41 | same error |
| `gcr.io/distroless/cc-debian13:nonroot` (`RELEASE_RUNTIME_CC`) | 2.41 | same error |

`build-config.env`'s TWO TRACKS rule says published artifacts are built on
and ship on the `RELEASE_*` track. The native bundle is built on the `DEV_*`
track and cannot load on the release runtime image. ADR-1346 records the
exception (maintainer answer Q2), and `docs/state.md` tracks the release-track
build as `T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27`.

### Rehearsing the release build on pull requests

The Dev Container PR gate already builds `libvmaf-build`. After it, the gate
now builds `build-deps` through the same script (layers reused, see above) and
runs the release script with the release job's `docker run` flags. Two things
differ from a release checkout:

- `actions/checkout` in a pull request fetches the merge commit at depth 1 and
  no tags, so `git describe --tags --long --match 'v*.*.*'` fails and Meson
  falls back to `core/meson.build`'s project version. That version equals
  `.release-please-manifest.json` only on a release commit; on `master` today
  the manifest reads `0.0.0` and `core/meson.build` `3.2.1`, so the verifier
  would reject the manifest version. The rehearsal therefore creates a local
  lightweight tag `v<manifest version>` on `HEAD` in the throwaway checkout,
  which gives the exact release shape: `vmaf --version` prints
  `v<version>-0-g<commit>` and the verifier's describe branch accepts it
  against the manifest version.
- `GITHUB_SHA` is the merge commit, which is what the checkout's `HEAD` is, so
  the new `GITHUB_SHA` check passes.

Reproduced on a tagless depth-1 checkout of the same commit (fetched as
`+<sha>:refs/remotes/pull/1/merge`, the pull-request shape): the step's own
commands read `0.0.0` from the manifest, `git describe` then printed
`v0.0.0-0-g<commit>`, and the release script passed with `Verified Linux
release runtime 0.0.0 (git describe exactly on the release tag: ...)` in 145 s;
the host-side `--verify` accepted the stamp. With the local tag deleted and
the CLI relinked, `vmaf --version` printed `3.2.1`, the Meson fallback, which
the verifier would reject against the manifest's `0.0.0`.

### Identity carried by the stage

`dev/Containerfile` writes `/etc/vmafx-dev-container` once, in `build-deps`,
with `image_title=vmaf-dev-mcp`. `libvmaf-build`, `go-build` and `dev-mcp`
inherit it unchanged, and so does `dev/Containerfile.runner`, which is built
from `vmaf-dev-mcp:local`. The `vmaf-sycl-arc-runner` title that ADR-1178
added to the gate's allowlist was never written by any image.

## Alternatives explored

- **`libvmaf-build` stage**: ~29.5 GB, 28 to 35 minutes, unpinned third-party
  downloads at release time. Rejected by Q1.
- **Debian 13 release-track builder**: would satisfy the two-track rule and
  load on glibc 2.41; needs its own image, marker and proof. Deferred by Q2.
- **Bring the Arc runner up**: rejected by the first popup; see the ADR's
  References.
- **Hosted runner, no container**: breaks ADR-1102.
- **Pull the GHCR image with a job-level `container:`**: the image would not
  come from the tag.
- **Verify-only rehearsal without a tag**: would prove everything except the
  version equality and exercise the verifier's fallback branch, which a
  release never takes; the local tag rehearses the branch a release takes.

## Open questions

- Build the native bundle on the Debian 13 release track before the final
  1.0.0 (`T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27`).
- Hosted timings for `build-deps` and the release compile come from the first
  rehearsal runs; the 60-minute limit assumes a hosted runner is no more than
  about ten times slower than the workstation measurement.

## Related

- ADRs: [ADR-1346](../adr/1346-hosted-slim-container-release-build.md), [ADR-1178](../adr/1178-dev-container-image-publish.md), [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md), [ADR-0819](../adr/0819-dev-container-ci-gate.md)
- Prior digest: [Research for ADR-1178](1178-dev-container-image-publish.md)
- PRs: release PR #1213 (v1.0.0-rc.1), #1570 (release-candidate versions in the native verifier)
