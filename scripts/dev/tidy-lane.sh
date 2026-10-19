#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# tidy-lane.sh — measure a clang-tidy ratchet lane inside the dev container.
#
# A lane's baseline (scripts/ci/tidy-baseline-<lane>.json) is defined against one
# toolchain image: the Ubuntu 26.04 dev container, which carries gcc-15, nvcc,
# hipcc and icpx, plus the clang-tidy 22 build the hosted `Tidy Ratchet` job
# installs (ADR-1471). Measured anywhere else the counts differ: a glibc 2.44
# host reports `misc-static-assert` for every runtime assert() in C++, and a
# host without hipcc lints the -ENOSYS stubs instead of the device bodies.
#
# The script copies the tracked files of this checkout (uncommitted edits
# included, ignored files not) into a throwaway container, configures and
# builds the lane there with `make tidy-ratchet-build`, and runs
# `make tidy-ratchet` (or `tidy-ratchet-write`). Nothing is mounted from the
# host and the checkout is never written to, except that --write copies the
# rewritten baseline back.
#
# Usage:
#   scripts/dev/tidy-lane.sh [options] LANE...   LANE: cpu clang cuda hip sycl arm64 all
#
# Options:
#   --write        rewrite the lane's baseline from the measurement
#   --only PATH    measure this translation unit only (repeatable; path from
#                  the repository root). With --write the ratchet tightens the
#                  allowance of these files and leaves the rest of the
#                  baseline as it is (ADR-1243); without it nothing is compared
#   --image NAME   dev image (default $VMAFX_DEV_IMAGE or vmaf-dev-mcp:local)
#   --jobs N       CPUs for the build and for clang-tidy (default
#                  $TIDY_LANE_JOBS or 8); the container is capped to N CPUs
#   --out DIR      where reports, logs and written baselines land (default
#                  ${XDG_CACHE_HOME:-~/.cache}/vmafx-tidy-lanes)
#   -h, --help     this text
#
# Exit: the highest ratchet code of the lanes run (0 match, 2 a file is above
# its baseline, 3 a file is below it, 4 a lane did not build or a translation
# unit did not parse, 5 usage or I/O, or the installed clang-tidy is not the
# version the lane's baseline was measured with); 1 when the container could
# not be prepared.
set -euo pipefail
export LC_ALL=C

# The clang-tidy major the hosted job installs (`llvm.sh 22`); a contract test
# keeps this, the workflow and the Makefile on the same number.
CLANG_TIDY_MAJOR=22
# TIDY_LANE_CLANG_TIDY and TIDY_LANE_WORK exist for the script's own tests,
# which drive the container side against stand-ins.
CLANG_TIDY_BIN_PATH="${TIDY_LANE_CLANG_TIDY:-/usr/bin/clang-tidy-${CLANG_TIDY_MAJOR}}"
# apt.llvm.org's archive key. A changed key fails the run instead of trusting
# whatever the server returns.
LLVM_APT_SIGNER_URL="https://apt.llvm.org/llvm-snapshot.gpg.key"
LLVM_APT_SIGNER_SHA256="8b2a587ffd672c4687e7581dad4b2f6c1bb2ad6b480cd9771ba2ff48e0b8c75d"

ALL_LANES="cpu clang cuda hip sycl arm64"
# The arm64 lane's cross toolchain, from the image's own distribution: the
# compilers the cross file names, the C library they link against, and the
# emulator meson runs its compiler sanity check with.
CROSS_PACKAGES="gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libc6-dev-arm64-cross qemu-user"
WORK="${TIDY_LANE_WORK:-/tmp/vmafx-tidy}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

IMAGE="${VMAFX_DEV_IMAGE:-vmaf-dev-mcp:local}"
JOBS="${TIDY_LANE_JOBS:-8}"
OUT="${XDG_CACHE_HOME:-$HOME/.cache}/vmafx-tidy-lanes"
WRITE=0
IN_CONTAINER=0
LANES=""
ONLY=()

die() {
  printf 'tidy-lane: %s\n' "$1" >&2
  exit "${2:-1}"
}

usage() {
  sed -n '5,41p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

add_lane() {
  case "$1" in
    all) LANES="$ALL_LANES" ;;
    cpu | clang | cuda | hip | sycl | arm64) LANES="${LANES:+$LANES }$1" ;;
    *) die "unknown lane '$1' (cpu, clang, cuda, hip, sycl, arm64, all)" 5 ;;
  esac
}

# A translation unit for --only: a path below the repository root, spelled so
# that it survives make's word splitting.
add_only() {
  case "$1" in
    /* | *..* | *[!A-Za-z0-9_./+-]*) die "--only takes a path from the repository root, got '$1'" 5 ;;
  esac
  [ -f "$REPO_ROOT/$1" ] || die "--only: no such file: $1" 5
  ONLY+=("--only" "$1")
}

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --write) WRITE=1 ;;
      --in-container) IN_CONTAINER=1 ;;
      --image | --jobs | --out | --only)
        [ $# -ge 2 ] || die "$1 needs a value" 5
        case "$1" in
          --image) IMAGE="$2" ;;
          --jobs) JOBS="$2" ;;
          --out) OUT="$2" ;;
          --only) add_only "$2" ;;
        esac
        shift
        ;;
      -h | --help)
        usage
        exit 0
        ;;
      -*) die "unknown option '$1'" 5 ;;
      *) add_lane "$1" ;;
    esac
    shift
  done
  [ -n "$LANES" ] || die "no lane given (cpu, clang, cuda, hip, sycl, arm64, all)" 5
  case "$JOBS" in
    '' | *[!0-9]* | 0) die "--jobs needs a positive integer, got '$JOBS'" 5 ;;
  esac
}

# ------------------------------------------------------------------ host side

# The tracked and the not-yet-added files of the checkout as a tar stream
# rooted at vmafx-tidy/src/. A tracked file deleted in the working tree is
# skipped with a warning (--ignore-failed-read), like the build would miss it.
source_tar() {
  git -C "$REPO_ROOT" ls-files -z --cached --others --exclude-standard |
    tar -C "$REPO_ROOT" --null --no-recursion --ignore-failed-read \
      --owner=0 --group=0 --transform 'flags=r;s,^,vmafx-tidy/src/,' \
      -T - -cf -
}

# The container's results go into a fresh staging directory first. The shared
# report directory ($OUT) keeps the files of earlier runs, so a baseline read
# from it after a run that wrote none (a rejected write, a container that died)
# would be a stale one and --write would copy it over the checkout's baseline
# (T-TIDY-LANE-WRITE-STALE-BASELINE-2026-10-06). Only a file the container
# produced in this run can reach scripts/ci/.
copy_results() {
  local cid="$1" lane stage status=0
  stage="$(mktemp -d)"
  mkdir -p "$OUT"
  docker cp "$cid:$WORK/out/." "$stage/" >/dev/null 2>&1 ||
    printf 'tidy-lane: the container produced no report\n' >&2
  if [ "$WRITE" -eq 1 ]; then
    for lane in $LANES; do
      if [ -f "$stage/tidy-baseline-$lane.json" ]; then
        cp "$stage/tidy-baseline-$lane.json" "$REPO_ROOT/scripts/ci/tidy-baseline-$lane.json"
        printf 'tidy-lane: wrote scripts/ci/tidy-baseline-%s.json\n' "$lane"
      else
        printf 'tidy-lane: this run produced no baseline for %s; %s is left as it is\n' \
          "$lane" "scripts/ci/tidy-baseline-$lane.json" >&2
        status=1
      fi
    done
  fi
  if ! cp -R "$stage/." "$OUT/" 2>/dev/null; then
    printf 'tidy-lane: could not copy the results into %s\n' "$OUT" >&2
    status=1
  fi
  rm -rf "$stage"
  return "$status"
}

run_on_host() {
  command -v docker >/dev/null || die "docker not found"
  docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    die "image '$IMAGE' not found; build it: docker compose -f dev/docker-compose.yml build dev-mcp"
  local write_flag=() cid rc=0
  [ "$WRITE" -eq 1 ] && write_flag=(--write)
  # shellcheck disable=SC2086  # $LANES is a validated, space-separated list
  cid="$(docker create --cpus "$JOBS" --user 0:0 --no-healthcheck --entrypoint /bin/bash "$IMAGE" \
    "$WORK/src/scripts/dev/tidy-lane.sh" --in-container --jobs "$JOBS" \
    "${write_flag[@]}" "${ONLY[@]}" $LANES)"
  # shellcheck disable=SC2064  # expand $cid now: it is unset when the trap runs
  trap "docker rm -f '$cid' >/dev/null 2>&1 || true" EXIT
  source_tar | docker cp - "$cid:/tmp" || die "could not copy the checkout into the container"
  docker start --attach "$cid" || rc=$?
  copy_results "$cid" || [ "$rc" -ne 0 ] || rc=1
  printf 'tidy-lane: reports and logs in %s\n' "$OUT"
  return "$rc"
}

# ------------------------------------------------------------- container side

# clang-tidy from apt.llvm.org's llvm-toolchain-<codename>-22: the package the
# hosted job installs through llvm.sh. An image that already carries it skips
# the download.
ensure_clang_tidy() {
  if "$CLANG_TIDY_BIN_PATH" --version 2>/dev/null | grep -qF "LLVM version ${CLANG_TIDY_MAJOR}."; then
    return 0
  fi
  local key=/etc/apt/trusted.gpg.d/apt.llvm.org.asc codename
  # shellcheck disable=SC1091  # the image's own os-release
  codename="$(. /etc/os-release && printf '%s' "$VERSION_CODENAME")"
  curl -fsSL --retry 3 --max-time 60 "$LLVM_APT_SIGNER_URL" -o "$key"
  printf '%s  %s\n' "$LLVM_APT_SIGNER_SHA256" "$key" | sha256sum --check --status ||
    die "apt.llvm.org key does not match the pinned SHA-256; refusing to install"
  printf 'deb [signed-by=%s] https://apt.llvm.org/%s/ llvm-toolchain-%s-%s main\n' \
    "$key" "$codename" "$codename" "$CLANG_TIDY_MAJOR" >/etc/apt/sources.list.d/llvm-tidy.list
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    "clang-tidy-${CLANG_TIDY_MAJOR}" >/dev/null
  "$CLANG_TIDY_BIN_PATH" --version | grep -qF "LLVM version ${CLANG_TIDY_MAJOR}." ||
    die "clang-tidy ${CLANG_TIDY_MAJOR} did not install"
}

# The clang lane builds with the clang of the llvm-toolchain-<codename>-22
# archive that ensure_clang_tidy() set up (it parses the same sources with the
# compiler libFuzzer belongs to); the libFuzzer runtime comes with
# libclang-rt-22-dev. An image that carries both skips the download.
ensure_clang_compiler() {
  if command -v "clang-${CLANG_TIDY_MAJOR}" >/dev/null &&
    ls /usr/lib/llvm-${CLANG_TIDY_MAJOR}/lib/clang/*/lib/linux/libclang_rt.fuzzer-*.a >/dev/null 2>&1; then
    return 0
  fi
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    "clang-${CLANG_TIDY_MAJOR}" "libclang-rt-${CLANG_TIDY_MAJOR}-dev" >/dev/null
  command -v "clang-${CLANG_TIDY_MAJOR}" >/dev/null ||
    die "clang-${CLANG_TIDY_MAJOR} did not install"
}

have_cross_toolchain() {
  command -v aarch64-linux-gnu-gcc >/dev/null && command -v qemu-aarch64 >/dev/null
}

# The aarch64 cross toolchain for the arm64 lane. An image that carries it
# skips the download.
ensure_cross_toolchain() {
  have_cross_toolchain && return 0
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  # shellcheck disable=SC2086  # a fixed list of package names
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    $CROSS_PACKAGES >/dev/null
  have_cross_toolchain || die "the aarch64 cross toolchain did not install"
}

# The sycl lane configures with icx / icpx, which need oneAPI's environment;
# the arm64 lane needs its cross toolchain and the clang lane its compiler.
lane_environment() {
  [ "$1" != arm64 ] || ensure_cross_toolchain
  [ "$1" != clang ] || ensure_clang_compiler
  [ "$1" = sycl ] || return 0
  set +u
  # shellcheck disable=SC1091  # provided by the image
  . /opt/intel/oneapi/setvars.sh --force >/dev/null
  set -u
}

# Configure and build one lane: compile_commands.json and the generated headers.
build_lane() {
  lane_environment "$1"
  make tidy-ratchet-build LANE="$1" TIDY_RATCHET_BUILD_DIR="$WORK/build-$1" \
    TIDY_RATCHET_JOBS="$JOBS"
}

# Measure one built lane against its baseline, or rewrite the baseline.
measure_lane() {
  local target=tidy-ratchet
  [ "$WRITE" -eq 1 ] && target=tidy-ratchet-write
  lane_environment "$1"
  make "$target" LANE="$1" TIDY_RATCHET_BUILD_DIR="$WORK/build-$1" \
    CLANG_TIDY_BIN="$CLANG_TIDY_BIN_PATH" \
    TIDY_RATCHET_ARGS="--jobs $JOBS --report $WORK/out/tidy-ratchet-$1.json ${ONLY[*]}"
}

# make reports a failing recipe as exit 2 whatever the tool returned, so the
# ratchet's code is read back from make's own "Error N" line.
ratchet_exit_code() {
  local code
  code="$(sed -n 's/^make: \*\*\* \[.*\] Error \([0-9][0-9]*\)$/\1/p' "$1" | tail -n 1)"
  printf '%s' "${code:-$2}"
}

# A baseline names the clang-tidy it was measured with, and the counts of two
# versions are not comparable. A check or a scoped write under another version
# stops here; a full --write is how the baselines move to a new version.
baseline_version_matches() {
  local lane="$1" installed="$2" baseline="scripts/ci/tidy-baseline-$1.json" recorded
  [ -f "$baseline" ] || return 0
  recorded="$(sed -n 's/^ *"clang_tidy_version": "\(.*\)",\{0,1\}$/\1/p' "$baseline" | head -n 1)"
  [ -n "$recorded" ] && [ "$recorded" != "$installed" ] || return 0
  if [ "$WRITE" -eq 1 ] && [ "${#ONLY[@]}" -eq 0 ]; then
    printf '   clang-tidy %s replaces %s as the version of this baseline\n' "$installed" "$recorded"
    return 0
  fi
  printf 'tidy-lane: clang-tidy %s is installed, but %s was measured with %s.\n' \
    "$installed" "$baseline" "$recorded"
  printf '           Counts of two versions are not comparable. Re-measure every lane with\n'
  printf '           "--write all" to move the baselines to %s, or use an image with %s.\n' \
    "$installed" "$recorded"
  return 1
}

# Build and measure one lane; returns the lane's exit code.
run_lane() {
  local lane="$1" version="$2" log="$WORK/out/$1.log" rc=0
  baseline_version_matches "$lane" "$version" || return 5
  printf '   configuring and building (log: %s.log)\n' "$lane"
  if ! (build_lane "$lane") >"$log" 2>&1; then
    tail -n 30 "$log"
    printf 'tidy-lane: lane %s did not build; nothing was measured\n' "$lane"
    return 4
  fi
  printf '   running clang-tidy over every translation unit\n'
  (measure_lane "$lane") >>"$log" 2>&1 || rc="$(ratchet_exit_code "$log" $?)"
  grep -E '^(tidy-ratchet|error:|warning:|notice:)' "$log" || tail -n 20 "$log"
  if [ "$WRITE" -eq 1 ] && [ "$rc" -eq 0 ]; then
    cp "scripts/ci/tidy-baseline-$lane.json" "$WORK/out/"
  fi
  return "$rc"
}

run_in_container() {
  if ! renice -n 10 -p $$ >/dev/null 2>&1; then
    echo "note: could not lower the priority of the lane run" >&2
  fi
  mkdir -p "$WORK/out"
  cd "$WORK/src"
  ensure_clang_tidy
  local lane rc worst=0 version
  version="$("$CLANG_TIDY_BIN_PATH" --version | sed -n 's/.*LLVM version \([0-9.]*\).*/\1/p')"
  for lane in $LANES; do
    printf '\n== tidy lane %s (clang-tidy %s, %s jobs)\n' "$lane" "$version" "$JOBS"
    if [ "$lane" = hip ]; then
      # scripts/ci/clang-tidy-hip.sh sends the .hip kernels to ROCm's clang-tidy.
      printf '   .hip kernels: %s\n' \
        "$("${ROCM_PATH:-/opt/rocm}/llvm/bin/clang-tidy" --version 2>/dev/null | sed -n '/LLVM version/s/^ *//p')"
    fi
    rc=0
    run_lane "$lane" "$version" || rc=$?
    printf '== tidy lane %s: exit %s\n' "$lane" "$rc"
    [ "$rc" -le "$worst" ] || worst="$rc"
  done
  return "$worst"
}

main() {
  parse_args "$@"
  if [ "$IN_CONTAINER" -eq 1 ]; then
    run_in_container
  else
    run_on_host
  fi
}

# One group: bash reads it whole before it runs anything in it, and the exit
# inside means it never reads past it. A checkout that changes under a lane in
# progress therefore cannot change the script mid-run.
{
  main "$@"
  exit
}
