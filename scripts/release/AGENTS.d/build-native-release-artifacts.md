---
paths:
  - scripts/release/build-native-release-artifacts.sh
  - scripts/release/tests/test-build-native-release-artifacts.sh
  - dev/Containerfile
  - .github/workflows/supply-chain.yml
invariant: Compile inside Debian 13 release-build stage only; patchelf $ORIGIN on staged CLI copy.
---
<!-- markdownlint-disable MD013 MD060 -->
# Native release artifact builder (`build-native-release-artifacts.sh`, ADR-1346)

## build-native-release-artifacts.sh (ADR-1346, ADR-1354)

Runs inside `release-build` stage image, never on runner host:
`supply-chain.yml` `build-artifacts` builds stage from release tag via
`scripts/ci/build-dev-container-stage.sh release-build`, then
`docker run --pull never --network none` as runner UID/GID (usually no
passwd entry in image -> HOME=/) with checkout mounted. Dev Container PR gate rehearses
same invocation with manifest version and local tag `v<version>` on HEAD.
`release-build` = Debian 13 release track (`RELEASE_BUILDER_BASE`, glibc
2.41, GCC 14.2, Meson 1.7, xxd, pinned patchelf), separate root of
`dev/Containerfile`, writes same ADR-1102 marker bytes as `build-deps`.
Never move release compile back to `build-deps` (Ubuntu 26.04 -> binds
`sqrtf@GLIBC_2.43`, bundle dead on Debian 13 / Ubuntu 24.04 / distroless
release runtime); `check-dev-container-build-secret.py` `STAGE_CALLERS`
and builder allowlist reject it.
Order fixed: `--assert` container marker first (host run compiles nothing),
`GITHUB_SHA` set -> `git rev-parse HEAD` must equal it (stamp records
`GITHUB_SHA` as `git_commit`; mismatch or unresolvable HEAD = exit 1 before
Meson), `patchelf` present, Meson build, stage, `--stamp`,
`verify-native-release-artifacts.sh`. Stage copies `build/tools/vmaf` then
`patchelf --set-rpath '$ORIGIN'` on copy only; build tree keeps Meson's
`$ORIGIN/../src`. Not `meson install`: strips build RUNPATH, writes
target `install_rpath` (empty); setting one changes every installed `vmaf`,
beyond bundle. `patchelf` pinned in `release-build` (`PATCHELF_VERSION`,
Debian 13 `0.18.0-1.4`; ADR-1354 moved it off `build-deps`);
`RELEASE_BUILDER_BASE` Debian release move -> move pin, same PR. Never drop
pin: release compile runs `--network none`, no install.
Meson flags keep hosted-era bundle: `--buildtype=release
-Denable_avx512=true -Denable_cuda=false -Denable_sycl=false` plus
`-Denable_dnn=disabled` (no-op in `release-build`, which has no ONNX
Runtime; pinned so a stage that ships ORT cannot make `libvmaf.so` NEED
`libonnxruntime.so.1`), `-Denable_tests=false` (load-bearing: Debian 13 GCC
14.2 hit random LTO internal compiler errors, once heap corruption in lto1,
linking the ~150 unit tests in 2 of 3 full builds; 0 of 22 test-free builds
failed; release ships no tests) and `CCACHE_DISABLE=1` (`release-build` has
no ccache; kept because a ccache stage as HOME=/ UID fails every compile
with `Permission denied`). `SOURCE_DATE_EPOCH` scoped
to build subshell; stamp keeps wall-clock `stamped_at`. Bundle floor measured
GLIBC_2.38 (`__isoc23_strtol` family) + GLIBCXX_3.4.30, so
`verify-native-artifacts` runs `ubuntu-24.04` (oldest hosted image that
loads it; 22.04 = glibc 2.35 fails) and also starts CLI on
`RELEASE_RUNTIME_CC` without `LD_LIBRARY_PATH` (proves RUNPATH `$ORIGIN`
there). Keep label pinned, never `ubuntu-latest`.

Test coverage:
`scripts/release/tests/test-build-native-release-artifacts.sh` (stub
`meson`, real ELF fixture chain linked with Meson's build-tree RUNPATH,
staged RUNPATH `$ORIGIN` + build tree untouched, failing-`patchelf` case,
`GITHUB_SHA` match / mismatch / abbreviated / unset / no-Git cases, no
Docker; host needs `cc`, `readelf`, `patchelf` — `ubuntu-26.04` runner
image ships all three).
