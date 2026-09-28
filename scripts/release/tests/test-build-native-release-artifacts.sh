#!/usr/bin/env bash
# Regression tests for the in-container native release build (ADR-1346, ADR-1354).
#
# Runs scripts/release/build-native-release-artifacts.sh against a throwaway
# Git repository with a stub `meson` on PATH. The stub records how it was
# called and, on `compile`, links a tiny real ELF libvmaf chain and CLI, so
# staging, the provenance stamp and the clean-environment verifier all run for
# real. The dev container is simulated through VMAFX_CONTAINER_MARKER, as in
# scripts/ci/tests/test-check-container-build.sh. No Docker is needed, but the
# host needs cc, readelf and patchelf (the ubuntu-26.04 runner image and the
# release-build stage carry all three): the staged CLI's RUNPATH is rewritten
# and inspected for real.
#
# Usage: bash scripts/release/tests/test-build-native-release-artifacts.sh
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

for tool in cc patchelf readelf; do
  if ! command -v "$tool" >/dev/null; then
    printf 'ERROR: %s is required to run these tests\n' "$tool" >&2
    exit 1
  fi
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0

check() {
  local description="$1"
  shift
  if "$@"; then
    printf 'PASS: %s\n' "$description"
    pass=$((pass + 1))
  else
    printf 'FAIL: %s\n' "$description" >&2
    fail=$((fail + 1))
  fi
}

# The marker dev/Containerfile bakes into every stage, release-build included.
marker="$scratch/etc-vmafx-dev-container"
printf '%s\n' 'vmafx_dev_container=1' 'image_title=vmaf-dev-mcp' \
  'containerfile=dev/Containerfile' 'source=https://github.com/VMAFx/vmafx' >"$marker"

# Stub meson. `setup` only records its arguments and environment; `compile`
# links a real SONAME chain unless STUB_MESON_MODE asks for a failure.
stub_bin="$scratch/bin"
mkdir -p "$stub_bin"
cat >"$stub_bin/meson" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
printf '%s|CCACHE_DISABLE=%s|SOURCE_DATE_EPOCH=%s\n' \
  "$*" "${CCACHE_DISABLE:-}" "${SOURCE_DATE_EPOCH:-}" >>"$STUB_MESON_LOG"
[ "$1" = compile ] || exit 0
[ "${STUB_MESON_MODE:-ok}" = fail ] && exit 42
mkdir -p build/src build/tools
printf 'int vmafx_fixture(void) { return 321; }\n' >build/libvmaf.c
printf '%s\n' '#include <stdio.h>' '#include <string.h>' \
  'int vmafx_fixture(void);' \
  'int main(int argc, char **argv) {' \
  '    if (argc == 2 && strcmp(argv[1], "--version") == 0 && vmafx_fixture() == 321) {' \
  '        puts("3.2.1");' \
  '        return 0;' \
  '    }' \
  '    return 1;' \
  '}' >build/vmaf.c
cc -fPIC -shared -Wl,-soname,libvmaf.so.3 -o build/src/libvmaf.so.3.0.0 build/libvmaf.c
ln -s libvmaf.so.3.0.0 build/src/libvmaf.so.3
[ "${STUB_MESON_MODE:-ok}" = short-chain ] || ln -s libvmaf.so.3 build/src/libvmaf.so
# Meson links the build-tree CLI with RUNPATH $ORIGIN/../src, as the
# v1.0.0-rc.1 asset still carried; the release script must rewrite it.
cc -o build/tools/vmaf build/vmaf.c -Lbuild/src -l:libvmaf.so.3.0.0 \
  -Wl,--enable-new-dtags -Wl,-rpath,'$ORIGIN/../src'
STUB
chmod +x "$stub_bin/meson"

# A patchelf that fails, put in front of the real one by with_failing_patchelf.
failing_patchelf_bin="$scratch/failing-patchelf"
mkdir -p "$failing_patchelf_bin"
printf '%s\n' '#!/usr/bin/env bash' 'echo "patchelf stub: refusing" >&2' 'exit 3' \
  >"$failing_patchelf_bin/patchelf"
chmod +x "$failing_patchelf_bin/patchelf"

# runpath_of FILE: the DT_RUNPATH value readelf reports, or nothing.
runpath_of() {
  LC_ALL=C readelf --dynamic -- "$1" | sed -n 's/.*(RUNPATH)[^[]*\[\(.*\)\]$/\1/p'
}
runpath_is() { [[ "$(runpath_of "$1")" == "$2" ]]; }
runs_without_library_path() { env -i PATH=/usr/bin:/bin "$1" --version >/dev/null; }
# shellcheck disable=SC2016 # $ORIGIN is for the dynamic loader, not the shell.
origin='$ORIGIN'

# new_repo <dir> — a committed tree holding only what the build script reads.
new_repo() {
  local dir="$1"
  mkdir -p "$dir/scripts/ci" "$dir/scripts/release" "$dir/model" "$dir/LICENSES" "$dir/core"
  cp -- "$REPO_ROOT/scripts/ci/check-container-build.sh" "$dir/scripts/ci/"
  cp -- "$REPO_ROOT/scripts/release/build-native-release-artifacts.sh" \
    "$REPO_ROOT/scripts/release/verify-native-release-artifacts.sh" "$dir/scripts/release/"
  printf '{"model": "fixture"}\n' >"$dir/model/vmaf_fixture.json"
  printf 'Apache-2.0 fixture\n' >"$dir/LICENSES/LicenseRef-Apache-2.0-u2netp.txt"
  git -C "$dir" -c init.defaultBranch=main init -q
  git -C "$dir" add -A
  GIT_COMMITTER_DATE='2001-02-03T04:05:06Z' GIT_AUTHOR_DATE='2001-02-03T04:05:06Z' \
    git -C "$dir" -c user.name=fixture -c user.email=fixture@example.invalid \
    -c core.hooksPath=/dev/null -c commit.gpgsign=false commit -q -m fixture
}

# run_build <dir> <marker> [args...] — exit status of the script in <dir>.
# GITHUB_SHA is the fixture's HEAD, as actions/checkout leaves it in CI, unless
# RUN_GITHUB_SHA overrides it; RUN_GITHUB_SHA='' runs with GITHUB_SHA unset.
# RUN_PATH_PREFIX, when set, goes in front of the stub meson on PATH.
RUN_PATH_PREFIX=''
run_build() {
  local dir="$1" marker_path="$2" sha
  shift 2
  sha="${RUN_GITHUB_SHA-$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)}"
  (
    cd "$dir"
    if [ -n "$sha" ]; then
      export GITHUB_SHA="$sha"
    else
      unset GITHUB_SHA
    fi
    env PATH="${RUN_PATH_PREFIX:+$RUN_PATH_PREFIX:}$stub_bin:$PATH" \
      STUB_MESON_LOG="$dir/meson.log" \
      VMAFX_CONTAINER_MARKER="$marker_path" \
      bash scripts/release/build-native-release-artifacts.sh "$@"
  ) >"$dir/run.log" 2>&1
}

expect_status() {
  local expected="$1"
  shift
  local rc=0
  run_build "$@" || rc=$?
  [ "$rc" -eq "$expected" ]
}

# with_github_sha <sha> <helper> [args...] — run a helper with RUN_GITHUB_SHA
# set for run_build ('' = GITHUB_SHA unset).
with_github_sha() {
  local RUN_GITHUB_SHA="$1"
  shift
  "$@"
}

# with_failing_patchelf <helper> [args...] — run a helper with a patchelf that
# exits 3 ahead of the real one on the build's PATH.
with_failing_patchelf() {
  local RUN_PATH_PREFIX="$failing_patchelf_bin"
  "$@"
}

expect_failure() {
  local rc=0
  run_build "$@" || rc=$?
  [ "$rc" -ne 0 ]
}

regular_nonempty() { [[ -f "$1" && ! -L "$1" && -s "$1" ]]; }
absent() { [[ ! -e "$1" ]]; }
lacks_line() { ! grep -qx -- "$1" "$2"; }

# --- positive: inside the container the full bundle is built and verified ---
good="$scratch/good"
new_repo "$good"
check 'container build exits 0' expect_status 0 "$good" "$marker" 3.2.1
for name in libvmaf.so libvmaf.so.3 libvmaf.so.3.0.0 vmaf models.tar.gz \
  container-build-provenance.txt; do
  check "stages $name as a regular non-empty file" regular_nonempty "$good/artifacts/$name"
done
# ADR-1354: unit tests stay out of the release build; GCC 14.2 on the Debian 13
# release track crashed at random while LTO-linking them.
check 'meson setup keeps the release flags and pins DNN and unit tests off' grep -q \
  '^setup build core --buildtype=release -Denable_avx512=true -Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled -Denable_tests=false|' \
  "$good/meson.log"
check 'meson runs with ccache disabled' grep -q '^compile -C build|CCACHE_DISABLE=1|' "$good/meson.log"
check 'meson sees SOURCE_DATE_EPOCH from the commit' grep -q \
  '^compile -C build|CCACHE_DISABLE=1|SOURCE_DATE_EPOCH=981173106$' "$good/meson.log"
check 'stamp records the dev-container identity' grep -qx \
  'image_title=vmaf-dev-mcp' "$good/artifacts/container-build-provenance.txt"
check 'stamp keeps a wall-clock stamped_at (epoch scoped to the build)' lacks_line \
  'stamped_at=2001-02-03T04:05:06Z' "$good/artifacts/container-build-provenance.txt"
check 'no u2netp license without the mirror binary' absent \
  "$good/artifacts/LicenseRef-Apache-2.0-u2netp.txt"
check 'stamp records the checked-out commit' grep -qx \
  "git_commit=$(git -C "$good" rev-parse HEAD)" "$good/artifacts/container-build-provenance.txt"

# --- the staged CLI carries RUNPATH $ORIGIN (T-RELEASE-NATIVE-RUNPATH-...) ---
check "staged vmaf has RUNPATH exactly $origin" runpath_is "$good/artifacts/vmaf" "$origin"
check "build-tree vmaf keeps Meson's RUNPATH $origin/../src" \
  runpath_is "$good/build/tools/vmaf" "$origin/../src"
check 'staged vmaf stays executable after the RUNPATH rewrite' test -x "$good/artifacts/vmaf"
check 'staged vmaf runs next to its library with no LD_LIBRARY_PATH' \
  runs_without_library_path "$good/artifacts/vmaf"
check 'the verifier confirms the RUNPATH' grep -qF \
  "chain and RUNPATH $origin." "$good/run.log"

failing_patchelf="$scratch/failing-patchelf-run"
new_repo "$failing_patchelf"
check 'a failing patchelf fails the build' \
  with_failing_patchelf expect_failure "$failing_patchelf" "$marker" 3.2.1
check 'a failing patchelf leaves no provenance stamp' absent \
  "$failing_patchelf/artifacts/container-build-provenance.txt"

# --- GITHUB_SHA must name the checked-out commit (the stamp records it) ---
local_run="$scratch/no-github-sha"
new_repo "$local_run"
check 'a run without GITHUB_SHA builds (local use)' \
  with_github_sha '' expect_status 0 "$local_run" "$marker" 3.2.1
check 'without GITHUB_SHA the stamp records HEAD' grep -qx \
  "git_commit=$(git -C "$local_run" rev-parse HEAD)" \
  "$local_run/artifacts/container-build-provenance.txt"

other_sha="$scratch/other-sha"
new_repo "$other_sha"
check 'GITHUB_SHA naming another commit exits 1' \
  with_github_sha 0123456789abcdef0123456789abcdef01234567 \
  expect_status 1 "$other_sha" "$marker" 3.2.1
check 'the mismatch names both commits' grep -q \
  "checked-out HEAD $(git -C "$other_sha" rev-parse HEAD) is not GITHUB_SHA 0123456789abcdef0123456789abcdef01234567" \
  "$other_sha/run.log"
check 'a mismatched GITHUB_SHA never invokes meson' absent "$other_sha/meson.log"
check 'a mismatched GITHUB_SHA creates no artifacts/' absent "$other_sha/artifacts"

short_sha="$scratch/short-sha"
new_repo "$short_sha"
check 'an abbreviated GITHUB_SHA is not the commit (exact match only)' \
  with_github_sha "$(git -C "$short_sha" rev-parse --short HEAD)" \
  expect_status 1 "$short_sha" "$marker" 3.2.1

no_git="$scratch/no-git"
new_repo "$no_git"
rm -rf "$no_git/.git"
check 'GITHUB_SHA without a Git checkout fails closed' \
  with_github_sha 0123456789abcdef0123456789abcdef01234567 \
  expect_status 1 "$no_git" "$marker" 3.2.1
check 'an unresolvable HEAD is reported' grep -q \
  'cannot resolve the checked-out commit' "$no_git/run.log"
check 'an unresolvable HEAD never invokes meson' absent "$no_git/meson.log"

# --- boundary: models.tar.gz is byte-reproducible across builds ---
again="$scratch/again"
new_repo "$again"
check 'second container build exits 0' expect_status 0 "$again" "$marker" 3.2.1
check 'models.tar.gz is byte-identical across builds' cmp -s \
  "$good/artifacts/models.tar.gz" "$again/artifacts/models.tar.gz"

# --- boundary: an untracked u2netp mirror is staged with its license ---
mirror="$scratch/mirror"
new_repo "$mirror"
printf 'onnx-bytes\n' >"$mirror/model/u2netp_mirror.onnx"
check 'build with the u2netp mirror exits 0' expect_status 0 "$mirror" "$marker" 3.2.1
check 'u2netp mirror is staged' regular_nonempty "$mirror/artifacts/u2netp_mirror.onnx"
check 'u2netp license is staged' regular_nonempty \
  "$mirror/artifacts/LicenseRef-Apache-2.0-u2netp.txt"

# --- negative: a host build is refused before anything is compiled ---
host="$scratch/host"
new_repo "$host"
check 'host build exits 1' expect_status 1 "$host" "$scratch/no/such/marker" 3.2.1
check 'host build never invokes meson' absent "$host/meson.log"
check 'host build creates no artifacts/' absent "$host/artifacts"

# --- negative: wrong version, failed compile, short SONAME chain ---
wrong="$scratch/wrong-version"
new_repo "$wrong"
check 'version mismatch fails the build' expect_failure "$wrong" "$marker" 3.2.2
check 'version mismatch is reported by the verifier' grep -q \
  "reported '3.2.1', expected '3.2.2'" "$wrong/run.log"

broken="$scratch/broken"
new_repo "$broken"
STUB_MESON_MODE=fail
export STUB_MESON_MODE
check 'compile failure fails the build' expect_failure "$broken" "$marker" 3.2.1
unset STUB_MESON_MODE
check 'compile failure writes no stamp' absent \
  "$broken/artifacts/container-build-provenance.txt"

short="$scratch/short-chain"
new_repo "$short"
STUB_MESON_MODE="short-chain"
export STUB_MESON_MODE
check 'incomplete SONAME chain exits 1' expect_status 1 "$short" "$marker" 3.2.1
unset STUB_MESON_MODE
check 'incomplete SONAME chain is named' grep -q \
  'incomplete Meson libvmaf SONAME chain' "$short/run.log"

# --- negative: invocation errors ---
usage="$scratch/usage"
new_repo "$usage"
check 'no version exits 64' expect_status 64 "$usage" "$marker"
check 'empty version exits 64' expect_status 64 "$usage" "$marker" ''
check 'extra argument exits 64' expect_status 64 "$usage" "$marker" 3.2.1 extra
check 'usage errors never invoke meson' absent "$usage/meson.log"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
