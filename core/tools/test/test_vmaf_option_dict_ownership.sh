#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# T-CLI-PRE-REGISTRATION-OPTS-DICT-LEAK-2026-09-30 — `vmaf` must release the
# option dictionaries of `--feature name=opt=val` and of a `--model` feature
# overload on every way a run can stop.
#
# The CLI keeps each dictionary in CLISettings until it hands it to the libvmaf
# call that takes it (vmaf_use_feature, vmaf_model_feature_overload,
# vmaf_model_collection_feature_overload). A run that stopped before that call,
# because an input could not be opened, the frame geometry was invalid, a model
# or an earlier feature failed its dimension check, the extractor name was
# unknown, or the feature was pinned to a backend the run did not start, used
# to exit with the dictionaries still allocated: LeakSanitizer reported 158
# bytes for `--feature cambi=full_ref=true`.
#
# Each case below must exit non-zero with its diagnostic and, in an ASan build,
# without a sanitizer report. The last case runs to completion and checks that
# the dictionaries libvmaf took are not freed a second time. In a build without
# LeakSanitizer the leak half of each check cannot fire, so the sanitizer jobs
# (`-Db_sanitize=address`) are the ones that guard it.
set -eu

BIN=./tools/vmaf
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_option_dict_ownership.scratch"
# MESON_SOURCE_ROOT is core/; the model JSON files sit one level above it.
MODEL_DIR="${MESON_SOURCE_ROOT:-${PWD}/..}/../model"
rm -rf "${WORK}"
mkdir -p "${WORK}"

# 64x64 4:2:0 8-bit, two frames of 64*64 + 2*32*32 = 6144 bytes. Too small for
# cambi (a side of 216 or more) and for the vmaf_v1.0.16 model, which uses it.
dd if=/dev/zero of="${WORK}/small.yuv" bs=6144 count=2 2>/dev/null
# 64x63 4:2:0: 64*63 + 2*32*32 = 6080 bytes a frame, so the reader accepts the
# file and the odd height is what stops the run.
dd if=/dev/zero of="${WORK}/odd.yuv" bs=6080 count=2 2>/dev/null

# The leak of a model-overload dictionary is hidden by a stale pointer that the
# conservative scan finds on the stack at exit; scan the heap and globals only.
LSAN_OPTIONS="${LSAN_OPTIONS:+${LSAN_OPTIONS}:}use_stacks=0:use_registers=0"
export LSAN_OPTIONS

FEATURE_OPTS="psnr=enable_chroma=true"
OVERLOAD="vif.vif_enhn_gain_limit=1.0"

fail() {
  echo "FAIL: $1" >&2
  echo "--- stderr ---" >&2
  cat "${WORK}/err.txt" >&2 || true
  exit 1
}

# run_case NAME EXPECTED_STATUS DIAGNOSTIC ARGS...
# EXPECTED_STATUS is "ok" (exit 0), "error" (non-zero, not a signal) or an
# exact exit code.
run_case() {
  name=$1
  expected=$2
  diagnostic=$3
  shift 3
  status=0
  "${BIN}" "$@" >/dev/null 2>"${WORK}/err.txt" || status=$?
  if grep -q -e "Sanitizer" -e "runtime error:" -e "double free" "${WORK}/err.txt"; then
    fail "${name}: sanitizer or allocator report"
  fi
  if [ "${expected}" = "ok" ]; then
    [ "${status}" = "0" ] || fail "${name}: exited ${status}, expected 0"
  elif [ "${expected}" != "error" ]; then
    [ "${status}" = "${expected}" ] || fail "${name}: exited ${status}, expected ${expected}"
  else
    [ "${status}" != "0" ] || fail "${name}: exited 0, expected an error"
    if [ "${status}" -gt 128 ] && [ "${status}" -le 192 ]; then
      fail "${name}: killed by signal $((status - 128))"
    fi
  fi
  if [ -n "${diagnostic}" ] && ! grep -q -e "${diagnostic}" "${WORK}/err.txt"; then
    fail "${name}: stderr lacks \"${diagnostic}\""
  fi
  echo "ok: ${name} (exit ${status})"
}

# 1. An input that cannot be opened: run_cli stops in open_cli_inputs().
run_case missing-input error "could not open file" \
  -r "${WORK}/missing.yuv" -d "${WORK}/missing.yuv" -w 64 -h 64 -p 420 -b 8 \
  --feature "${FEATURE_OPTS}" --model "path=${MODEL_DIR}/vmaf_v0.6.1.json:${OVERLOAD}"

# 2. An odd height for 4:2:0: the same stop, from the geometry check.
run_case odd-height error "odd height 63 not allowed" \
  -r "${WORK}/odd.yuv" -d "${WORK}/odd.yuv" -w 64 -h 63 -p 420 -b 8 \
  --feature "${FEATURE_OPTS}" --model "path=${MODEL_DIR}/vmaf_v0.6.1.json:${OVERLOAD}"

# 3. A model whose features do not fit the frame: load_cli_models() stops
#    before the overload and before any feature is registered.
run_case model-dimensions error "requires feature 'cambi'" \
  -r "${WORK}/small.yuv" -d "${WORK}/small.yuv" -w 64 -h 64 -p 420 -b 8 \
  --feature "${FEATURE_OPTS}" \
  --model "path=${MODEL_DIR}/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json:${OVERLOAD}"

# 4. The first feature fails its dimension check; the second one's options
#    were never handed over.
run_case feature-dimensions error "feature 'cambi' needs width or height" \
  -r "${WORK}/small.yuv" -d "${WORK}/small.yuv" -w 64 -h 64 -p 420 -b 8 \
  --no_prediction --feature cambi --feature "${FEATURE_OPTS}"

# 5. An unknown extractor name: vmaf_use_feature() hands the options back.
run_case unknown-extractor error "problem loading feature extractor: no_such_extractor" \
  -r "${WORK}/small.yuv" -d "${WORK}/small.yuv" -w 64 -h 64 -p 420 -b 8 \
  --no_prediction --feature "no_such_extractor=some_option=1"

# 6. An option the extractor does not have: vmaf_use_feature() took and freed
#    the options, so the CLI must not free them again.
run_case unknown-option error "unknown option 'no_such_option'" \
  -r "${WORK}/small.yuv" -d "${WORK}/small.yuv" -w 64 -h 64 -p 420 -b 8 \
  --no_prediction --feature "psnr=no_such_option=1"

# 7. A backend-pinned twin the run cannot use: --backend cpu starts no GPU
#    backend, so the CLI refuses psnr_cuda (ADR-0498, exit 100) before it
#    registers anything. LeakSanitizer used to turn that exit 100 into 1.
run_case pinned-backend-refused 100 "refusing to silently fall back to CPU" \
  -r "${WORK}/small.yuv" -d "${WORK}/small.yuv" -w 64 -h 64 -p 420 -b 8 \
  --backend cpu --no_prediction --feature psnr_cuda=enable_chroma=false

# 8. A complete run: every dictionary goes to libvmaf and none is freed twice.
run_case complete-run ok "" \
  -r "${WORK}/small.yuv" -d "${WORK}/small.yuv" -w 64 -h 64 -p 420 -b 8 \
  --feature "${FEATURE_OPTS}" --model "path=${MODEL_DIR}/vmaf_v0.6.1.json:${OVERLOAD}" \
  --json -o "${WORK}/out.json"

rm -rf "${WORK}"
echo "PASS: option dictionaries are released on every exit path"
