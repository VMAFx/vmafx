# Change Log

> The Unreleased section tracks VMAFx changes. Release-please turns these
> entries and Conventional Commits into ordinary SemVer releases.

## [Unreleased]
### Fixed

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
