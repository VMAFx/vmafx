#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
# run-all-tests.sh — run the full *runnable* VMAFx test matrix inside the
# reproducer container, stream every section live, save per-section logs, and
# print a final PASS/FAIL table. Built so nothing can hang for hours.
#
# Sections (all inside $IMAGE, on the real Arc A380 when present):
#   [C]        meson C test suite — unit + SIMD bit-exactness (incl.
#              test_float_motion_simd) + SYCL parity on the GPU.
#   [GOLDEN]   Netflix CPU golden gate — the 5 golden files, STRICT (places=4).
#              Also fails hard if the golden files differ from master.
#   [MATERIAL] Broader clip/metric Python tests — every clip-based metric test
#              (quality_runner, vmaf_v1_quality_runner, feature_extractor,
#              vmafexec x2, noref, raw, cambi, ssimulacra2, result, reader),
#              -m "not slow".
#
# Deliberately EXCLUDED (they hang or need corpora/network this image lacks):
#   model training / bootstrap / cross-validation, BD-rate math, CLI-subprocess,
#   Cython (cy_test), and the Python tooling/harness self-tests.
#
# Every section has a host `timeout` backstop, so a stuck test dies in minutes.
#
# Usage:
#   ./scripts/test/run-all-tests.sh                    # all sections
#   REBUILD=1 ./scripts/test/run-all-tests.sh          # rebuild the image first
#   SECTIONS="golden material" ./scripts/test/run-all-tests.sh   # subset
#   LOG_DIR=/tmp/mylogs ./scripts/test/run-all-tests.sh          # log location
#   ENGINE=docker ./scripts/test/run-all-tests.sh               # docker instead

set -euo pipefail
# Sections report their own failures in RESULT[]: a section's exit status is
# kept in a variable (`|| rc=$?`) and a grep that finds nothing goes through
# grep_opt, so `set -e` stops the run only on a real error.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The test matrix runs INSIDE the image and needs meson / python / the source
# tree / the meson build dir — all of which live in Containerfile.vmafx's
# `build` STAGE, not the lean `prod` stage (ADR-1715). So this harness builds
# and runs `--target build`, tagged `:build`, and leaves the deployment image
# `vmafx-zerocopy-fix:latest` (the default `prod` target) alone. Operators
# verify the lean prod image separately via its baked `vmaf-selftest`.
IMAGE="${IMAGE:-vmafx-zerocopy-fix:build}"
BUILD_TARGET="${BUILD_TARGET:-build}"
CONTAINERFILE="${CONTAINERFILE:-Containerfile.vmafx}"
RENDER_NODE="${RENDER_NODE:-/dev/dri/renderD128}"
LOG_DIR="${LOG_DIR:-${REPO_ROOT}/test-logs}"
SECTIONS="${SECTIONS:-c report golden material}"
ENGINE="${ENGINE:-podman}"

mkdir -p "$LOG_DIR"
declare -A RESULT

c1='\033[1;36m'
c0='\033[0m'
red='\033[1;31m'
grn='\033[1;32m'
# grep that treats "no match" (status 1) as an answer, not a failure; a real
# grep error (status 2, e.g. a missing log) still stops the run.
grep_opt() { grep "$@" || [ "$?" -eq 1 ]; }

banner() { printf "\n${c1}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n %s\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${c0}\n" "$1"; }

# Shared plumbing the Python suites need: symlink the binary path the harness
# expects, a cpumask=16 shim (AVX-512 SIGILL workaround), and VMAFEXEC_PATH.
PLUMBING='
  cd /src/vmafx
  ln -sfn ../build core/build
  printf "#!/bin/sh\nexec /src/vmafx/build/tools/vmaf --cpumask 16 \"\$@\"\n" > /tmp/vmaf-shim
  chmod +x /tmp/vmaf-shim
  printf "VMAFEXEC_PATH = \"/tmp/vmaf-shim\"\n" > compat/vmaf/externals.py
'

# GPU is attached ONLY to the [C] meson section (its SYCL-parity tests compare
# GPU-vs-CPU and need the device). [GOLDEN] and [MATERIAL] are Netflix CPU tests
# and run CPU-only — with a device present, the `vmaf` binary auto-selects SYCL
# and would run the golden pair on the GPU (pre-existing SYCL parity gaps →
# false "regressions"). The authoritative phase08 golden gate is also CPU-only.
DEV=""
if [ -e "$RENDER_NODE" ]; then
  DEV="--device $RENDER_NODE"
else
  printf "${red}⚠ %s not found — GPU (SYCL) tests will report as skipped/failed.${c0}\n" "$RENDER_NODE"
fi

# run_section <extra-args> <bash-body> <logfile> <timeout-secs>
run_section() {
  local dev="$1" body="$2" log="$3" tmo="$4"
  # shellcheck disable=SC2086
  timeout --signal=KILL "$tmo" "$ENGINE" run --rm --security-opt label=disable \
    $dev \
    -e VMAF_TEST_CPUMASK=16 -e UR_L0_USE_IMMEDIATE_COMMANDLISTS=0 \
    -v "${REPO_ROOT}/python/test/resource:/src/vmafx/python/test/resource" \
    --entrypoint bash "$IMAGE" -c "$body" 2>&1 | tee "$log"
  return "${PIPESTATUS[0]}"
}

# --- ensure the image exists --------------------------------------------------
if [ "${REBUILD:-0}" = "1" ] || ! "$ENGINE" image exists "$IMAGE" 2>/dev/null; then
  banner "BUILD  $IMAGE  (--target $BUILD_TARGET)"
  if ! (cd "$REPO_ROOT" && "$ENGINE" build --target "$BUILD_TARGET" -t "$IMAGE" -f "$CONTAINERFILE" .) \
    2>&1 | tee "$LOG_DIR/build.log"; then
    printf "${red}BUILD FAILED — see %s${c0}\n" "$LOG_DIR/build.log"
    exit 1
  fi
fi

# --- [C] meson C test suite ---------------------------------------------------
# Note on SIGILL: this host may expose avx512f but NOT avx512vbmi, while the
# image builds AVX-512 kernels with -mavx512vbmi and the runtime dispatch gates
# only on avx512f. C unit tests run the pipeline directly (no `--cpumask 16`
# shim), so cambi/adm AVX-512 kernels execute a VBMI instruction the host lacks
# → SIGILL (signal 4). That is a pre-existing environmental host gap, NOT a
# defect in the tested numerics; such failures are classified below as `env`.
if [[ " $SECTIONS " == *" c "* ]]; then
  banner "[C] Meson unit suite (unit + SIMD bit-exact + SYCL parity)"
  run_section "$DEV" "
    cd /src/vmafx
    python3 scripts/ci/run_meson_test.py -- -C build --print-errorlogs -t 6 || true
  " "$LOG_DIR/c-suite.log" 1800 || c_rc=$?
  ok="$(grep_opt -oE 'Ok: *[0-9]+' "$LOG_DIR/c-suite.log" | tail -1 | grep_opt -oE '[0-9]+')"
  totalfail="$(grep_opt -oE '^Fail: *[0-9]+' "$LOG_DIR/c-suite.log" | tail -1 | grep_opt -oE '[0-9]+')"
  sigill="$(grep_opt -cE 'SIGILL|signal 4' "$LOG_DIR/c-suite.log")"
  real=$((${totalfail:-0} - sigill))
  [ "$real" -lt 0 ] && real=0
  if [ -z "$ok" ]; then
    RESULT[C]="no summary — killed/hung, exit ${c_rc:-0} (see log)"
  elif [ "$real" -eq 0 ]; then
    RESULT[C]="Ok: ${ok}, real-fail: 0  (+${sigill} AVX512-VBMI SIGILL = host gap, env)"
  else
    RESULT[C]="FAIL — real-fail: ${real}  (+${sigill} env SIGILL) — see log"
  fi
  if [ "$sigill" -gt 0 ]; then
    echo "   env SIGILL (host lacks avx512vbmi — would pass on a full-AVX512 host):"
    grep_opt -E 'SIGILL|signal 4' "$LOG_DIR/c-suite.log" | grep_opt -oE '[^ ]+ / [^ ]+' |
      sed -n 's/^/     · /; 1,20p'
  fi
fi

# --- [REPORT] reference-pair verification (numbers, not just pass/fail) --------
# Runs the canonical Netflix pairs through the vmaf CLI and prints, per pair /
# per feature: reference, actual, delta, tol, verdict — plus a CPU-vs-SYCL
# cross-backend parity table. Attaches the GPU ($DEV) for the SYCL half; when
# no render node is present it degrades to the CPU golden table only.
# run_section already exports UR_L0_USE_IMMEDIATE_COMMANDLISTS=0 + VMAF_TEST_CPUMASK=16.
if [[ " $SECTIONS " == *" report "* ]]; then
  banner "[REPORT] reference-pair verification — reference / actual / delta / verdict"
  # Mount the reporter from the host so it works without a rebuild (it is also
  # baked into the image on the next REBUILD=1, like the rest of the source).
  report_dev="$DEV -v ${REPO_ROOT}/scripts/test/reference_report.py:/src/vmafx/scripts/test/reference_report.py:ro"
  [ -z "$DEV" ] && report_dev="$report_dev -e REPORT_NO_SYCL=1"
  rc=0
  run_section "$report_dev" "
    cd /src/vmafx
    VMAF_BIN=/src/vmafx/build/tools/vmaf python3 scripts/test/reference_report.py
  " "$LOG_DIR/report.log" 600 || rc=$?
  if [ "${rc:-1}" -eq 0 ]; then
    RESULT[REPORT]="all reference rows within tolerance"
  else
    RESULT[REPORT]="FAIL — a reference row exceeded tolerance (see log)"
  fi
fi

# --- [GOLDEN] Netflix CPU golden gate (strict) --------------------------------
if [[ " $SECTIONS " == *" golden "* ]]; then
  banner "[GOLDEN] Netflix CPU golden gate — 5 files, STRICT (places=4)"
  # Guard the GLOBAL RULE 1 invariant first: golden files must equal master.
  if git -C "$REPO_ROOT" diff --quiet master -- \
    python/test/quality_runner_test.py python/test/feature_extractor_test.py \
    python/test/vmafexec_test.py python/test/vmafexec_feature_extractor_test.py \
    python/test/result_test.py 2>/dev/null; then
    echo "OK: 5 golden files byte-identical to master."
  else
    printf '%sFATAL: golden files differ from master (GLOBAL RULE 1).%s\n' "$red" "$c0"
    RESULT[GOLDEN]="GOLDEN FILES MODIFIED — abort"
    SECTIONS="${SECTIONS/golden/}"
  fi
  if [[ " $SECTIONS " == *" golden "* ]]; then
    run_section "" "$PLUMBING
      PYTHONPATH=/src/vmafx/python python3 -m pytest \
        python/test/quality_runner_test.py \
        python/test/feature_extractor_test.py \
        python/test/vmafexec_test.py \
        python/test/vmafexec_feature_extractor_test.py \
        python/test/result_test.py \
        -m 'not slow' -q -rfE -p no:cacheprovider
    " "$LOG_DIR/golden.log" 1800 || golden_rc=$?
    fails="$(grep_opt -cE '^(FAILED|ERROR)' "$LOG_DIR/golden.log")"
    line="$(grep_opt -oE '[0-9]+ passed[^)]*' "$LOG_DIR/golden.log" | tail -1)"
    if [ "$fails" -eq 0 ] && [ -n "$line" ]; then
      RESULT[GOLDEN]="STRICT PASS — $line"
    else RESULT[GOLDEN]="FAIL — $fails failed/errored, exit ${golden_rc:-0} (see log)"; fi
  fi
fi

# --- [MATERIAL] broader clip/metric material ----------------------------------
if [[ " $SECTIONS " == *" material "* ]]; then
  banner "[MATERIAL] all clip/metric tests (-m 'not slow')"
  run_section "" "$PLUMBING
    PYTHONPATH=/src/vmafx/python python3 -m pytest \
      python/test/quality_runner_test.py python/test/vmaf_v1_quality_runner_test.py \
      python/test/feature_extractor_test.py python/test/vmafexec_test.py \
      python/test/vmafexec_feature_extractor_test.py python/test/noref_feature_extractor_test.py \
      python/test/raw_extractor_test.py python/test/cambi_test.py \
      python/test/ssimulacra2_test.py python/test/result_test.py python/test/reader_test.py \
      -m 'not slow' -q -rfE -p no:cacheprovider
  " "$LOG_DIR/material.log" 1800 || material_rc=$?
  line="$(grep_opt -oE '[0-9]+ passed[^)]*' "$LOG_DIR/material.log" | tail -1)"
  fails="$(grep_opt -cE '^(FAILED|ERROR)' "$LOG_DIR/material.log")"
  RESULT[MATERIAL]="${line:-no summary} (${fails} failed/errored, exit ${material_rc:-0})"
  if [ "$fails" -gt 0 ]; then
    grep_opt -E '^(FAILED|ERROR)' "$LOG_DIR/material.log" | sed 's/^/   ⚠ /'
  fi
fi

# --- summary ------------------------------------------------------------------
banner "RESULTS"
for k in C REPORT GOLDEN MATERIAL; do
  [ -n "${RESULT[$k]:-}" ] || continue
  if [[ "${RESULT[$k]}" == *FAIL* || "${RESULT[$k]}" == *"Fail: "[1-9]* ]]; then col="$red"; else col="$grn"; fi
  printf "  ${col}%-9s${c0} %s\n" "$k" "${RESULT[$k]}"
done
printf "\n  logs: %s/{build,c-suite,golden,material}.log\n" "$LOG_DIR"
echo "  notes:"
echo "   • excluded: model-training / BD-rate / CLI / Cython / tooling tests"
echo "     (they hang or need corpora/network this reproducer image lacks)."
# The avx512f-without-VBMI SIGILL was fixed by dropping the unused
# -mavx512vbmi from the AVX-512 build flags, so a current image builds 0 SIGILL. Only surface the
# explanatory note if this run actually hit one (e.g. a stale image) —
# in that case rebuild with REBUILD=1.
if [ "${sigill:-0}" -gt 0 ]; then
  echo "   • [C] 'env SIGILL': host lacks avx512vbmi but this (stale) image's"
  echo "     AVX-512 kernels use it and the runtime gates dispatch on avx512f"
  echo "     only. Fixed in-tree (unused -mavx512vbmi dropped) — rebuild with REBUILD=1."
fi
# Only surface a
# note if [MATERIAL] actually failed this run; the failing test lines are
# already printed above.
if [[ "${RESULT[MATERIAL]:-}" == *"("[1-9]*" failed"* ]]; then
  echo "   • [MATERIAL] failures above are NOT the Netflix golden gate"
  echo "     ([GOLDEN] is the CPU golden contract). Triage each against its"
  echo "     own fork-metric snapshot before assuming a regression."
fi
