#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# fetch-test-yuvs.sh — download the canonical Netflix golden-pair YUVs that
# back python/test/quality_runner_test.py and feature_extractor_test.py.
#
# Background
# ----------
# Netflix removed these binaries from the upstream repo in 2020 (commit
# bac8b6073) and moved them to a sibling repository at
#   https://github.com/Netflix/vmaf_resource
# The Python test suite still references them via VmafConfig.test_resource_path,
# so any local clone that wants to run the Python tests must provision them
# under python/test/resource/yuv/ (which is .gitignored).
#
# CI jobs that need the pairs run this script, and running it once after
# cloning makes `pytest python/test/` pass on the canonical pair (subject to
# the usual scipy/skimage env).
#
# Usage: fetch-test-yuvs.sh [NAME...]
#   No argument: every fixture of FIXTURES. With arguments: only the named
#   ones (a name that is not in FIXTURES is refused).
#
# A download that fails for a transient reason (HTTP 5xx, a dropped
# connection) is retried for minutes: compat/python-vmaf/tools/retrying_download.py,
# the one retry policy this script shares with the Python harness's
# download_reactively(). The first file that still fails ends the run; the
# others come from the same host.
#
# Fixture md5sums are checked after download so a corrupted or substituted
# file is detected at provision time, not as a confusing 2x-drift in test
# assertions later. (That's exactly how this script came to exist: a stale
# local copy of src01_hrc00_576x324.yuv with the right name and size but
# wrong content was producing 84 test failures whose root cause was not
# obvious from any assertion message — see ADR-0493.)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
YUV_DIR="${REPO_ROOT}/python/test/resource/yuv"
BASE_URL="https://github.com/Netflix/vmaf_resource/raw/master/python/test/resource/yuv"
DOWNLOAD="${REPO_ROOT}/compat/python-vmaf/tools/retrying_download.py"

# Canonical files + expected md5 (verified 2026-06-08 against vmaf_resource HEAD).
# Format: "filename md5"
#
# The 3 golden-gate test pairs (CLAUDE.md §8):
#   1. Normal:               src01_hrc00 ↔ src01_hrc01           (576×324)
#   2. Checkerboard 1-px:   checkerboard_…0_0 ↔ checkerboard_…1_0   (1920×1080)
#   3. Checkerboard 10-px:  checkerboard_…0_0 ↔ checkerboard_…10_0  (1920×1080)
FIXTURES=(
  "src01_hrc00_576x324.yuv b16f67d34de8535d45fa5ed80586747f"
  "src01_hrc01_576x324.yuv 2e02bed10fa706c17a0b1b026ac05807"
  "checkerboard_1920_1080_10_3_0_0.yuv ad14c75d1897e7f2bc72a882c32e49a6"
  "checkerboard_1920_1080_10_3_1_0.yuv 0e290566458d800c534ab18103619d43"
  "checkerboard_1920_1080_10_3_10_0.yuv 289119e1168ab656ac79487df8d307b9"
  # 576x324, 1 frame, yuv420p10le, PQ / BT.2020nc / limited range (the files carry
  # no colorimetry): the HDR conversion fixtures of python/test/vmafexec_color_args_test.py,
  # added to Netflix/vmaf_resource 2026-10-03 (commit 5e7b853ba), BSD-2-Clause-Patent.
  "ref_dock-na-HDR_res2160_rate5p0_maxRate100000_bufSize200000_fps59p940_576x324_1frame.yuv 14391e847683cc27c7f39768afa3ffda"
  "dis_dock-na-HDR_res1440_rate15p0_maxRate10000_bufSize20000_fps59p940_576x324_1frame.yuv 6665d2b17cb15ede58d5f31eddd7a753"
)

# Print the FIXTURES entries of the named fixtures, all of them without a name.
# Returns 2 on a name that is no fixture.
select_fixtures() {
  local wanted entry found
  if [ "$#" -eq 0 ]; then
    printf '%s\n' "${FIXTURES[@]}"
    return 0
  fi
  for wanted in "$@"; do
    found=""
    for entry in "${FIXTURES[@]}"; do
      if [ "${entry% *}" = "${wanted}" ]; then
        found="${entry}"
      fi
    done
    if [ -z "${found}" ]; then
      echo "ERROR: ${wanted} is not a fixture of this script (see FIXTURES)" >&2
      return 2
    fi
    printf '%s\n' "${found}"
  done
}

# Provision one "name md5" entry into YUV_DIR and count it in main's ok, fixed,
# fetched or failed. Returns 1 when its download failed, which ends the run.
provision() {
  local name="${1% *}" expected_md5="${1##* }" actual_md5 target
  target="${YUV_DIR}/${name}"

  if [ -s "${target}" ]; then
    actual_md5="$(md5sum "${target}" | awk '{print $1}')"
    if [ "${actual_md5}" = "${expected_md5}" ]; then
      echo "ok      ${name} (md5 verified)"
      ok=$((ok + 1))
      return 0
    fi
    echo "stale   ${name} (md5 ${actual_md5}, want ${expected_md5}) — refetching"
    rm -f "${target}"
    fixed=$((fixed + 1))
  fi

  echo "fetch   ${name}"
  # -I: the script's own directory (vmaf/tools) stays off the module path.
  if ! python3 -I "${DOWNLOAD}" "${BASE_URL}/${name}" "${target}"; then
    echo "ERROR: download failed for ${name}; not trying the fixtures after it" >&2
    failed=$((failed + 1))
    return 1
  fi

  actual_md5="$(md5sum "${target}" | awk '{print $1}')"
  if [ "${actual_md5}" != "${expected_md5}" ]; then
    echo "ERROR: ${name} md5 mismatch after download (got ${actual_md5}, want ${expected_md5})" >&2
    echo "       Either upstream content drifted, or the expected md5 in this script is stale." >&2
    failed=$((failed + 1))
    return 0
  fi
  fetched=$((fetched + 1))
}

# Provision the named fixtures (all of FIXTURES without a name) into YUV_DIR.
main() {
  local listing entry
  local ok=0 fixed=0 fetched=0 failed=0

  listing="$(select_fixtures "$@")" || return 2
  mkdir -p "${YUV_DIR}"

  while IFS= read -r entry; do
    provision "${entry}" || break
  done <<<"${listing}"

  echo
  echo "fetch-test-yuvs.sh: ${ok} already-ok, ${fixed} replaced, ${fetched} fetched, ${failed} failed"

  [ "${failed}" -eq 0 ]
}

# Run when executed. scripts/ci/tests/test_fetch_test_yuvs.py sources this file
# instead, to call main() with a stand-in server and a fixture list of its own.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
