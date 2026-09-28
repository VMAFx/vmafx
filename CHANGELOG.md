# Change Log

> The Unreleased section tracks VMAFx changes. Release-please turns these
> entries and Conventional Commits into ordinary SemVer releases.

## [Unreleased]

## [1.0.0-rc.2] - 2026-09-28
### Changed

- The native Linux release bundle (`vmaf` and `libvmaf.so*`) now runs on
  Ubuntu 24.04, Debian 13 and newer distributions: it needs glibc 2.38 and the
  libstdc++ of GCC 12 instead of glibc 2.43. It is compiled on the fork's
  Debian 13 release track, the base of the published container images, and
  each release checks it on Ubuntu 24.04 and in the distroless `cc-debian13`
  runtime image. Ubuntu 22.04 and Debian 12 remain unsupported (ADR-1354).


- **The first-release candidate plan moved back by one candidate.**
  `v1.0.0-rc.2` is a stabilisation candidate: it ships the dependency updates
  and fixes merged since rc.1, and testers use the same report kit
  (`tools/rc1-tester/`) and exit bar as for rc.1. Benchmarking, profiling and
  tuning move to `v1.0.0-rc.3`, and the one-shot model retrain moves to
  `v1.0.0-rc.4`, so each phase number now matches its tag. The release guide,
  roadmap, tester guide, retrain runbook and `vmaf-rc1-report list-tools`
  inventory show the new mapping (ADR-1352).


### Fixed

- `--model` and `--feature` values keep their backslashes, so Windows paths work
  as typed: `path=..\..\models\m.json`, `path=\\server\share\m.json` and
  `path=C:\models\.cache\m.json` used to lose a backslash each (`\.` and `\\`
  were escapes in values too). `\:` and `\=` still escape a delimiter, and a
  backslash run directly before `:` or `=`, or at the end of a value, is read in
  pairs so a backslash in front of a delimiter stays writable. Keys and
  overload names keep the full escape set (ADR-1355). If you wrote a UNC path
  as `\\\\server\share` per the earlier advice, write `\\server\share` now.


- Container publication finishes for the large images. The GPU image jobs free
  runner disk before `syft` scans the pushed image (the 1.0.0-rc.1 ROCm SBOM
  failed with "no space left on device" after the image was pushed and
  signed), and the two-platform `vmafx-operator` build gets 60 minutes instead
  of 30. The CPU, MCP server, node and controller images no longer compile
  libvmaf's unit-test suite, which they never shipped: the arm64 builds were
  cancelled at 60 minutes while still linking tests under emulation.


- **Nightly Kubernetes E2E scores again** — the kind + kuttl scoring smoke sent
  a 64x64 clip to `/v1/score`. Since `vmaf_v1.0.16_3d0h` became the default
  model (ADR-1169), libvmaf refuses input that small (`cambi` needs one side of
  at least 216 pixels, `speed_chroma` needs 4:2:0 luma of at least 160x160), so
  the server answered HTTP 500 and every scheduled E2E run from 2026-09-24 on
  failed. The fixtures are now 216x160, the smallest size the default model
  accepts, and the test ConfigMap is created with server-side apply because the
  larger pair exceeds the 256 KiB annotation that client-side apply writes. The
  always-on E2E contract test now checks the fixture size against the
  thresholds in `core/src/feature/` on every pull request. On failure the score
  script now prints the `/v1/score` error body and the server Pods' logs; it
  previously discarded the body and, through `deployment/vmafx`, printed the
  operator's logs instead.


- The FFmpeg patched by `ffmpeg-patches/0019` builds without warnings on
  aarch64. GCC 14.2 flagged two `-Wstringop-overflow` false positives in
  `libavcodec/a64multienc.c`, which the node image's warning gate rejects, so
  the arm64 `vmafx-node` image could not build. The index tables are now
  filled per palette interval, with identical results. An image recovery run
  also takes `ffmpeg-patches/` from the dispatching commit (ADR-1350).


- **ffmpeg calls work with FFmpeg 9 again**: FFmpeg 9 removed the `-vsync`
  option, so the Python harness's decode step and the `describe_worst_frames`
  MCP tool (Python and Go servers) failed with "Unrecognized option 'vsync'"
  on the FFmpeg release this project pins. They now pass
  `-fps_mode passthrough`, the same mode as the old `-vsync 0`, which makes
  FFmpeg 5.1 the oldest release they work with. The harness change ports
  Netflix/vmaf `aeaf2877d`.


- The `vmafx-operator` and `vmafx-server` release images build each
  architecture on its own native runner, like `vmafx-node` (ADR-1349). Their
  arm64 halves were emulated with QEMU and took 30 to 46 minutes of a 60-minute
  limit; each still publishes one signed, attested multi-arch image.


- **Helm: the server Deployment no longer selects the operator and node Pods.**
  The chart's server Deployment and StatefulSet selected only the release
  labels, so they also matched the operator, node and `helm test` Pods, and
  `kubectl logs deployment/vmafx` could print the operator's log. Both now
  also select `app.kubernetes.io/component: server` (ADR-1353). Scoring
  traffic was not affected: the Services already selected the server Pods
  only. **Upgrade note for v1.0.0-rc.1 installs:** a workload's selector
  cannot be changed in place, so `helm upgrade` fails with
  `spec.selector: ... field is immutable`. Delete the server workload first;
  `--cascade=orphan` keeps its Pods serving until the upgrade replaces them:
  `kubectl delete deployment,statefulset -n <namespace> --cascade=orphan -l app.kubernetes.io/instance=<release>,app.kubernetes.io/component=server`,
  then run `helm upgrade` as usual. Uninstalling and installing again also
  works. See "Upgrading from 1.0.0-rc.1" in
  `docs/development/k8s-deployment.md`.


- A published release's container images can be recovered after a build
  recipe fix (ADR-1347). A `workflow_dispatch` of the image publish workflows
  on the default branch builds the release tag's source with that commit's
  `docker/` recipe and labels each image with `io.vmafx.build-recipe`; the
  dispatch path also reads the prerelease flag from the release itself, so it
  works for release candidates.
  The release guide now covers the `release-publish` environment step a
  recovery run needs (the environment admits only `v*` tags, so `master` is
  allowed for the recovery and removed afterwards) and how to make a new GHCR
  package public, which the REST API cannot do.
  The post-push smoke tests verify each image's signature against the
  identity of the run that signed it; they required the tag identity, which a
  recovery run cannot produce, so the first v1.0.0-rc.1 recovery failed its
  CPU smoke test after pushing and signing the image. The release guide shows
  how to verify a recovered image (`@refs/heads/master` identity and its
  `io.vmafx.build-recipe` label).


- `test_meson_secret_env_sanitization` passes when Meson is installed with
  `pip install --user`, which `scripts/setup/ubuntu.sh` and the nightly
  ThreadSanitizer job both do. Its probes replaced `HOME` with a temporary
  directory, which also moved Python's per-user package directory, so every
  probe stopped at `No module named 'mesonbuild'` before Meson ran. The probes
  now keep `PYTHONUSERBASE` pointed at the real user base while `HOME` stays
  synthetic.


- The `vmafx-node` image bundles rclone at `/usr/local/bin/rclone` again, as the
  storage guide states. The node resolves `s3://`, `gs://`, `rclone://` and
  `remote:path` inputs by running rclone (ADR-0719), but `docker/Dockerfile.node`
  never installed it, so every remote input failed. The static binary comes
  from the official rclone image, pinned by digest in `build-config.env`, and
  the release smoke test now runs it.


- The `vmafx-node` release image builds again. Its arm64 half compiled FFmpeg,
  libvmaf and the node binary under QEMU emulation and never finished within
  the job's two-hour limit, so no node image was published for v1.0.0-rc.1.
  Each architecture now builds on its own native runner, and the release
  publishes one merged multi-arch image, signed, attested and with an SBOM
  (ADR-1349).


- Release candidates after `1.0.0-rc.1` are numbered `1.0.0-rc.2`,
  `1.0.0-rc.3`, and so on (ADR-1348). release-please used its default
  versioning, which turned the first fix after `1.0.0-rc.1` into a proposed
  `1.0.1-rc.1` (release PR #1575); it now uses `prerelease` versioning, and the
  final cut becomes `1.0.0` once `prerelease` is switched off. The release guide
  also no longer claims that every new GHCR package starts private: with the
  organization's public-package setting on, a package first pushed from this
  repository is created public.
- The container quick start works for release candidates: it names the
  release tag instead of `latest` (release candidates are never tagged
  `latest`) and passes `--pixel_format 420` instead of the rejected `yuv420p`.
  The image docs list the `-rocm10` variant and the exact signing identities
  for recovered images, and a recovered image's
  `org.opencontainers.image.revision` label names the tag's source commit
  rather than the recipe commit it was built with.
- The container images carry the built-in models again: the CPU, MCP-server,
  CUDA and oneAPI builders lacked `xxd`, so libvmaf silently embedded no model
  and scoring without `--model` failed. The oneAPI image now installs the
  Unified Memory Framework runtime its SYCL adapters need; without it the image
  found no SYCL device. The publish smoke tests now score with the default model
  and check the oneAPI adapters, and the GPU image docs give working device and
  group flags, forced-backend scoring examples and measured parity figures.


- The native Linux `vmaf` CLI attached to a release runs next to the
  downloaded `libvmaf.so*` files without `LD_LIBRARY_PATH`. The
  `v1.0.0-rc.1` CLI kept Meson's build-tree RUNPATH `$ORIGIN/../src`, so it
  found `libvmaf.so.3` only when `LD_LIBRARY_PATH` pointed at the download
  directory. The release build now sets the staged CLI's RUNPATH to exactly
  `$ORIGIN`, and the release gate runs the CLI without `LD_LIBRARY_PATH` and
  rejects any other RUNPATH.


- libvmaf builds against libc++ 23 again. The vendored libsvm
  (`core/src/svm.cpp`) defined its own global `swap` template, and libc++ 23's
  `std::vector` internals now call `swap` unqualified, so both it and
  `std::swap` matched and the file failed with "call to 'swap' is ambiguous".
  libsvm now uses `std::swap`; scores are unchanged.


- **Whole-tree clang-tidy ratchet ignores generated build products**: `tidy-ratchet.py`
  now skips every translation unit, diagnostic and header under `--build-dir`, so a
  build directory inside the repository measures the same checked-in sources as one
  outside it. The nightly `Full clang-tidy scan` builds in `build/` and had been failing
  on the 18 `xxd`-generated model embeds (`build/src/*.json.c`,
  `build/src/brisque_live.model.c`, two `misc-use-internal-linkage` warnings each)
  that the cpu baseline no longer lists. `make tidy-ratchet` / `tidy-ratchet-write` with
  the default in-tree `core/build` no longer measure or record them either. The arm64
  baseline, recorded from an in-tree `build-arm64`, was re-measured on its own toolchain:
  764 to 615 warnings (36 generated-file warnings, 25 already-ignored Pelorus-mirror
  entries and 88 warnings cleaned since 2026-09-23; no count rose) (ADR-1142).

## [1.0.0-rc.1] - 2026-09-27

This release collects 2790 changelog entries.
They are recorded in full, unedited, in
[`docs/changelog-archive/1.0.0-rc.1.md`](docs/changelog-archive/1.0.0-rc.1.md) — too long to read inline here.

| Section | Entries |
| --- | --- |
| Changed | 625 |
| Added | 510 |
| Removed | 14 |
| Fixed | 1585 |
| Security | 56 |
