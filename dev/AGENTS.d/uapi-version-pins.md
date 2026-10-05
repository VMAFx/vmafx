---
paths:
  - dev/Containerfile
  - dev/scripts/dev-mcp-entrypoint.sh
  - dev/scripts/fetch-intel-neo.py
  - build-config.env
invariant: NEO and ROCm match host kernel ABI; build-config.env owns pins; entrypoint probe runs argv without shell.
---
<!-- markdownlint-disable MD013 -->
# Userspace ↔ host-kernel UAPI version pins (ADR-0541)

Intel NEO compute-runtime and ROCm KFD userspace MUST match host
kernel's i915 / xe / KFD ioctl ABI, or `vmaf --backend sycl|hip`
silently falls back to CPU. Two hard pins live in
`dev/Containerfile`:

- **Shared `INTEL_NEO_VERSION`** and shared `LEVEL_ZERO_VERSION`.
  Pinned via GitHub releases: Intel's `noble/unified` APT repo's
  newest as of 2026-05-18 = `25.18.x`, too old for kernel ≥ 7.0.
  Level Zero loader comes from `oneapi-src/level-zero`. NEO and
  Level Zero `RUN`s source copied root `build-config.env` from
  `/opt/vmafx/`; both Level Zero download URL components use
  `LEVEL_ZERO_VERSION`. Do not reintroduce separate
  `LEVEL_ZERO_VER`/`LEVEL_ZERO_VERSION` ARG or literal version.
  `scripts/ci/check-workflow-versions.py` and single-source fixture
  gate protect this consumer. Renovate owns setting only in
  `build-config.env`. **Invariant (ADR-1145, ADR-1360)**:
  `INTEL_NEO_VERSION` in `build-config.env` = only pinned Intel NEO
  version; never reintroduce `ARG NEO_VER`, `GMMLIB_VER` or `IGC_VER`.
  Matching `gmmlib`, `IGC` and `intel-ocloc` debs dynamically derived,
  verified against published sha256 checksums at build time by
  `dev/scripts/fetch-intel-neo.py`. CI SYCL build hosts install same
  release's ocloc via `scripts/ci/install-intel-ocloc.sh`
  (`--components ocloc`); oneAPI release image uses same script with
  `--components build` / `runtime` (ADR-1368) = same fetcher, same package
  set as this Containerfile. `intel-ocloc` load-bearing: icpx runs `ocloc`
  for SYCL `spir64_gen` AOT; no ocloc -> `meson setup` refuses. IGC debs
  land in `/usr/local/lib`, no ldconfig trigger -> keep `ldconfig` after
  NEO install.
  **GitHub credential transport (ADR-1271):** optional API token is   BuildKit secret `github_token`, exposed as `GITHUB_TOKEN` only to NEO
  fetch `RUN`. Never reintroduce `ARG GITHUB_TOKEN`, `ENV GITHUB_TOKEN`, or   token-valued `--build-arg`. Raw anonymous builds omit `--secret`; Compose
  maps host variable and treats unset or empty input as anonymous. Keep
  `scripts/ci/check-dev-container-build-secret.py`, its fixture tests, and   Docker/Compose `--check` workflow steps wired together.
- **Digest-pinned `rocm-src` stage**
  (`rocm/dev-ubuntu-26.04:10.1.0-full`) replaces old `ARG ROCM_VER` +
  `repo.radeon.com/rocm/apt/` install. **Invariant (ADR-1225 /
  ADR-1231)**: keep selected digest-pinned image as SDK source.
  Update `ROCM_BUILDER` / `ROCM_RUNTIME` in `build-config.env`,
  regenerate their mirrors. ADR-1225 records historical
  package-channel checks; those observations aren't current
  inventory of AMD's release channels. **Invariant**: keep prune
  list in `rocm-src` stage, but never prune
  `librocprofiler-register` — `libamdhip64.so` links it, dropping it
  makes every HIP binary fail at load with "cannot open shared
  object file". Keep post-prune HIP compile/link and host-entry
  smoke in this stage; `hipconfig --version` alone doesn't exercise
  compiler dependency closure. Smoke compiles kernel but doesn't
  launch it or require GPU. ROCm 6.x KFD ioctls don't match kernel ≥
  7.0; 10.0.0 / 10.1.0 do (verified on Linux 7.2.3 / 7.2.9 with
  `gfx1036`). **Invariant (ROCm bump):** Renovate moves only the image
  pins. Same PR: `ROCM_VERSION`; `core-<major>.<minor>` paths in
  `rocm-src` stage + `docker/Dockerfile.node`; prune list here = `EXCLUDES`
  of `scripts/ci/install-rocm-from-image.sh`, every entry must still match;
  LLVM sonames in `tools/rc1-tester/image/hip-runtime.json`; TheRock /
  rocm-systems / llvm-project pins in `tools/rc1-tester/image/licensing.json`
  (from `share/therock/therock_manifest.json`); HIP device suite rerun on
  `gfx1036` (compiler can move: 10.1.0 = LLVM 24).

CI / maintainer's host running newer kernel that breaks these pins
-> `dev-mcp-entrypoint.sh` runtime-visibility probe (also ADR-0541)
surfaces regression on container start as `WARN: SYCL GPU NOT detected` or
`WARN: HIP HSA GPU agent NOT detected`. Bump relevant version owner, rebuild.

**probe runs argv, never shell string.** `_probe_with_retry` in
`scripts/dev-mcp-entrypoint.sh` takes one program name and runs it as
`"${prog}"`. earlier `eval "${cmd}"` form was removed by PR #350, came
back through stale squash-merge (PR #414) and was removed again on
2026-09-19: entrypoint is PID 1 with container's whole environment,
so probe value that ever comes from configuration would be command
injection. probe that needs flags gets explicit argv handling in function; do not reintroduce `eval` or `bash -c`. Keep function at top
level with opening line `_probe_with_retry() {` —
`scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh` (pre-commit hook
`test-dev-mcp-entrypoint-probe`) extracts it by that line. Detection regexes
are anchored to full runtime records: SYCL accepts only leading
`[level_zero:gpu...]` or `[opencl:gpu...]` record; HIP accepts only full
`Name: gfx...` or `Device Type: GPU` line. Do not loosen these to token
searches that can turn initialization diagnostic into false success.
