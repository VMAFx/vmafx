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
# Usage: fetch-test-yuvs.sh [--bundle NAME] [NAME...]
#   No argument: every fixture of FIXTURES. With arguments: only the named
#   ones (a name that is not in FIXTURES is refused).
#   --bundle <name>: provision fixture bundle into python/test/resource/yuv/<name>/
#   (supported: pq-sollevante-v1).
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
YUV_DIR="${YUV_DIR:-${REPO_ROOT}/python/test/resource/yuv}"
BASE_URL="${BASE_URL:-https://github.com/Netflix/vmaf_resource/raw/master/python/test/resource/yuv}"
DOWNLOAD="${REPO_ROOT}/compat/python-vmaf/tools/retrying_download.py"

BUNDLE_NAME="pq-sollevante-v1"
BUNDLE_ARCHIVE="${BUNDLE_NAME}.tar.zst"
BUNDLE_BASE_URL="${BUNDLE_BASE_URL:-https://github.com/VMAFx/vmafx/releases/download/fixtures-${BUNDLE_NAME}}"
BUNDLE_ARCHIVE_SHA256="${BUNDLE_ARCHIVE_SHA256:-f7546a16c3190c841e09a5de83d98db86156a06869fb9fbc58ff86307db2f416}"

if [ -z "${BUNDLE_MEMBERS+x}" ]; then
  BUNDLE_MEMBERS=(
    "CREDIT.txt 877cfc3bca59e377ecf6cddb059d82e33de3d8074bdae523fdb3398b0f10fc74"
    "LICENCE-CC-BY-4.0.txt 7d3512159f970cdcf296e8404be05947228d0bfaf72801cb5b3077f4330f71f9"
    "sollevante_dis_1920x1080_8f.yuv fddfa3e86f9db381f9d14b189405416f7b854dcb4289b71daddba756cbe67050"
    "sollevante_ref_1920x1080_8f.yuv 1be6007ebe12a52d5c4ccd891ec3dbc9935d0481a094eb63f3b214cd944089ee"
  )
fi

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
  # 576x324, 48 frames, yuv422p10le: the src01 pair of the device gate's
  # src01-tagged-576x324-422-10 row (core/test/test_gpu_v1_models_no_fallback.py);
  # md5 read 2026-10-31 from Netflix/vmaf_resource 5e7b853ba, BSD-2-Clause-Patent.
  "src01_hrc00_576x324.yuv422p10le.yuv 91fa924e0c63c7ecb5bf85fae006edf9"
  "src01_hrc01_576x324.yuv422p10le.yuv 4de6d217e67bd29e45a6f2f5358131fe"
  # 480x270, yuv420p10le: the 10-bit input of the bit-exact tests' all-or-none
  # fixture set (core/test/vmafx_fixture_util.h vt_inputs[3]);
  # md5 read 2026-10-31 from Netflix/vmaf_resource 5e7b853ba, BSD-2-Clause-Patent.
  "sparks_ref_480x270.yuv42010le.yuv 81e1a8f7725f15b42cb5ae022dbab7d1"
  "sparks_dis_480x270.yuv42010le.yuv 5cd74c216330d5eb8d0e2155945a18d2"
)

# Print the FIXTURES entries of the named fixtures, all of them without a name.
# Returns 2 on a name that is no fixture.
select_fixtures() {
  local wanted entry found
  if [ "$#" -eq 0 ]; then
    [ "${#FIXTURES[@]}" -eq 0 ] && return 0
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

# Verify that all member files exist in the target directory with matching sha256.
verify_bundle_members() {
  local target_dir="$1"
  local entry file expected actual path

  for entry in "${BUNDLE_MEMBERS[@]}"; do
    file="${entry% *}"
    expected="${entry##* }"
    path="${target_dir}/${file}"
    if [ ! -f "${path}" ]; then
      return 1
    fi
    actual="$(sha256sum "${path}" | awk '{print $1}')"
    if [ "${actual}" != "${expected}" ]; then
      return 1
    fi
  done
  return 0
}

# Check tar archive for path traversal or absolute paths before extraction.
check_path_traversal() {
  local archive="$1"
  local listing member

  if ! listing="$(tar -tf "${archive}" 2>/dev/null)"; then
    echo "ERROR: failed to inspect archive contents: ${archive}" >&2
    return 1
  fi

  while IFS= read -r member; do
    [ -z "${member}" ] && continue
    if [[ "${member}" =~ ^/ ]] || [[ "${member}" =~ (^|/)\.\.(/|$) ]] || [[ "${member}" =~ \\ ]]; then
      echo "ERROR: archive member contains illegal path or traversal: ${member}" >&2
      return 1
    fi
  done <<<"${listing}"
  return 0
}

# Extract bundle into staging area, verify member sha256 sums, and install to target.
extract_and_verify_bundle() {
  local bundle_name="$1" tmp_archive="$2" stage_dir="$3" target_dir="$4"
  local src_dir

  if ! tar -xf "${tmp_archive}" -C "${stage_dir}"; then
    echo "ERROR: failed to extract ${bundle_name}" >&2
    return 1
  fi

  src_dir="${stage_dir}"
  if [ -d "${stage_dir}/${bundle_name}" ]; then
    src_dir="${stage_dir}/${bundle_name}"
  fi

  if ! verify_bundle_members "${src_dir}"; then
    echo "ERROR: ${bundle_name} member sha256 mismatch after extraction" >&2
    return 1
  fi

  rm -rf "${target_dir}"
  mkdir -p "${target_dir}"
  cp -a "${src_dir}/." "${target_dir}/"
  return 0
}

# Provision a fixture bundle into YUV_DIR/<bundle_name>.
provision_bundle() {
  local bundle_name="$1"
  local target_dir="${YUV_DIR}/${bundle_name}"
  local bundle_url="${BUNDLE_URL:-${BUNDLE_BASE_URL}/${BUNDLE_ARCHIVE}}"
  local tmp_archive stage_dir actual_sha

  if [ -d "${target_dir}" ] && verify_bundle_members "${target_dir}"; then
    echo "ok      bundle ${bundle_name} (sha256 verified)"
    return 0
  fi

  if [ -d "${target_dir}" ]; then
    echo "stale   bundle ${bundle_name} — refetching"
    rm -rf "${target_dir}"
  fi

  mkdir -p "${YUV_DIR}"
  tmp_archive="$(mktemp "${YUV_DIR}/.${bundle_name}.tar.zst.XXXXXX")"
  stage_dir="$(mktemp -d "${YUV_DIR}/.${bundle_name}.stage.XXXXXX")"

  echo "fetch   bundle ${bundle_name}"
  if ! python3 -I "${DOWNLOAD}" "${bundle_url}" "${tmp_archive}"; then
    echo "ERROR: download failed for bundle ${bundle_name}" >&2
    rm -f "${tmp_archive}"
    rm -rf "${stage_dir}" "${target_dir}"
    return 1
  fi

  actual_sha="$(sha256sum "${tmp_archive}" | awk '{print $1}')"
  if [ "${actual_sha}" != "${BUNDLE_ARCHIVE_SHA256}" ]; then
    echo "ERROR: ${bundle_name} archive sha256 mismatch (got ${actual_sha}, want ${BUNDLE_ARCHIVE_SHA256})" >&2
    rm -f "${tmp_archive}"
    rm -rf "${stage_dir}" "${target_dir}"
    return 1
  fi

  if ! check_path_traversal "${tmp_archive}"; then
    echo "ERROR: ${bundle_name} archive failed security validation" >&2
    rm -f "${tmp_archive}"
    rm -rf "${stage_dir}" "${target_dir}"
    return 1
  fi

  if ! extract_and_verify_bundle "${bundle_name}" "${tmp_archive}" "${stage_dir}" "${target_dir}"; then
    rm -f "${tmp_archive}"
    rm -rf "${stage_dir}" "${target_dir}"
    return 1
  fi

  rm -f "${tmp_archive}"
  rm -rf "${stage_dir}"
  echo "ok      bundle ${bundle_name} (extracted and verified)"
  return 0
}

# Parse command line options into global variables PARSED_BUNDLE and PARSED_FIXTURES.
parse_cli_args() {
  PARSED_BUNDLE=""
  PARSED_FIXTURES=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --bundle=*)
        PARSED_BUNDLE="${1#*=}"
        shift
        ;;
      --bundle)
        [ "$#" -ge 2 ] || {
          echo "ERROR: --bundle requires an argument" >&2
          return 2
        }
        PARSED_BUNDLE="$2"
        shift 2
        ;;
      *)
        PARSED_FIXTURES+=("$1")
        shift
        ;;
    esac
  done
}

# Provision the named fixtures (all of FIXTURES without a name) into YUV_DIR.
main() {
  local listing entry
  local ok=0 fixed=0 fetched=0 failed=0

  parse_cli_args "$@" || return 2

  if [ -n "${PARSED_BUNDLE}" ]; then
    if [ "${PARSED_BUNDLE}" != "${BUNDLE_NAME}" ]; then
      echo "ERROR: ${PARSED_BUNDLE} is not a recognized bundle (supported: ${BUNDLE_NAME})" >&2
      return 2
    fi
    if ! provision_bundle "${PARSED_BUNDLE}"; then
      return 1
    fi
    if [ "${#PARSED_FIXTURES[@]}" -eq 0 ]; then
      return 0
    fi
  fi

  listing="$(select_fixtures "${PARSED_FIXTURES[@]}")" || return 2
  mkdir -p "${YUV_DIR}"

  while IFS= read -r entry; do
    [ -z "${entry}" ] && continue
    provision "${entry}" || break
  done <<<"${listing}"

  echo
  echo "fetch-test-yuvs.sh: ${ok} already-ok, ${fixed} replaced, ${fetched} fetched, ${failed} failed"

  [ "${failed}" -eq 0 ]
}

# Run when executed. scripts/ci/tests/test_fetch_test_yuvs.py sources this file
# instead, to call main() with a stand-in server and a fixture list of its own.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  if [ "${1:-}" = "--print-bundle-sha" ]; then
    echo "${BUNDLE_ARCHIVE_SHA256}"
    exit 0
  fi
  main "$@"
fi
