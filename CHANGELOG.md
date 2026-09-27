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


- `test_meson_secret_env_sanitization` passes when Meson is installed with
  `pip install --user`, which `scripts/setup/ubuntu.sh` and the nightly
  ThreadSanitizer job both do. Its probes replaced `HOME` with a temporary
  directory, which also moved Python's per-user package directory, so every
  probe stopped at `No module named 'mesonbuild'` before Meson ran. The probes
  now keep `PYTHONUSERBASE` pointed at the real user base while `HOME` stays
  synthetic.


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
