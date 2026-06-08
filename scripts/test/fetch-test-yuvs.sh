#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
# GitHub Actions does this at CI time via inline curl in
# .github/workflows/tests-and-quality-gates.yml. This script is the local
# equivalent — running it once after cloning makes `pytest python/test/`
# pass on the canonical pair (subject to the usual scipy/skimage env).
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
)

mkdir -p "${YUV_DIR}"

ok=0
fixed=0
fetched=0
failed=0

for entry in "${FIXTURES[@]}"; do
  name="${entry% *}"
  expected_md5="${entry##* }"
  target="${YUV_DIR}/${name}"

  if [ -s "${target}" ]; then
    actual_md5="$(md5sum "${target}" | awk '{print $1}')"
    if [ "${actual_md5}" = "${expected_md5}" ]; then
      echo "ok      ${name} (md5 verified)"
      ok=$((ok + 1))
      continue
    fi
    echo "stale   ${name} (md5 ${actual_md5}, want ${expected_md5}) — refetching"
    rm -f "${target}"
    fixed=$((fixed + 1))
  fi

  echo "fetch   ${name}"
  if ! curl -fsSL --retry 5 --retry-delay 2 --retry-all-errors \
    -o "${target}" "${BASE_URL}/${name}"; then
    echo "ERROR: download failed for ${name}" >&2
    failed=$((failed + 1))
    continue
  fi

  actual_md5="$(md5sum "${target}" | awk '{print $1}')"
  if [ "${actual_md5}" != "${expected_md5}" ]; then
    echo "ERROR: ${name} md5 mismatch after download (got ${actual_md5}, want ${expected_md5})" >&2
    echo "       Either upstream content drifted, or the expected md5 in this script is stale." >&2
    failed=$((failed + 1))
    continue
  fi
  fetched=$((fetched + 1))
done

echo
echo "fetch-test-yuvs.sh: ${ok} already-ok, ${fixed} replaced, ${fetched} fetched, ${failed} failed"

if [ "${failed}" -gt 0 ]; then
  exit 1
fi
