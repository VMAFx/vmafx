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
#   (supported: pq-sollevante-v1, hdr-ictcp-pixels-v1).
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

# The bundles this script provisions; scripts/ci/check_fixture_bundle_jobs.py reads
# this line, so a workflow cannot fetch a bundle the script does not know.
SUPPORTED_BUNDLES=("pq-sollevante-v1" "hdr-ictcp-pixels-v1")

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
    printf '%s\n' ${FIXTURES[@]+"${FIXTURES[@]}"}
    return 0
  fi
  for wanted in "$@"; do
    found=""
    for entry in ${FIXTURES[@]+"${FIXTURES[@]}"}; do
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

# Print the sha256 of a file; macOS ships shasum and not always sha256sum.
file_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1"
  else
    shasum -a 256 "$1"
  fi
}

# Return the expected archive sha256 for a recognized bundle name.
get_bundle_sha() {
  local bundle_name="$1"
  if [ -n "${BUNDLE_ARCHIVE_SHA256:-}" ]; then
    echo "${BUNDLE_ARCHIVE_SHA256}"
    return 0
  fi
  case "${bundle_name}" in
    pq-sollevante-v1)
      echo "f7546a16c3190c841e09a5de83d98db86156a06869fb9fbc58ff86307db2f416"
      ;;
    hdr-ictcp-pixels-v1)
      echo "b5cc0aff25a507fccd4808c8c7c4692db3bdf1a68bd3e77c6f081c5398d4ab1a"
      ;;
    *)
      return 1
      ;;
  esac
}

# Return the member list ("filename sha256") for a recognized bundle name;
# a caller-set BUNDLE_MEMBERS array overrides the built-in list.
get_bundle_members() {
  local bundle_name="$1"
  local entry
  if [ -z "${BUNDLE_MEMBERS+x}" ]; then
    builtin_bundle_members "${bundle_name}"
    return
  fi
  for entry in ${BUNDLE_MEMBERS[@]+"${BUNDLE_MEMBERS[@]}"}; do
    printf '%s\n' "${entry}"
  done
}

# Built-in member list of each supported bundle.
builtin_bundle_members() {
  local bundle_name="$1"
  case "${bundle_name}" in
    pq-sollevante-v1)
      printf '%s\n' \
        "CREDIT.txt 877cfc3bca59e377ecf6cddb059d82e33de3d8074bdae523fdb3398b0f10fc74" \
        "LICENCE-CC-BY-4.0.txt 7d3512159f970cdcf296e8404be05947228d0bfaf72801cb5b3077f4330f71f9" \
        "sollevante_dis_1920x1080_8f.yuv fddfa3e86f9db381f9d14b189405416f7b854dcb4289b71daddba756cbe67050" \
        "sollevante_ref_1920x1080_8f.yuv 1be6007ebe12a52d5c4ccd891ec3dbc9935d0481a094eb63f3b214cd944089ee"
      ;;
    hdr-ictcp-pixels-v1)
      printf '%s\n' \
        "CREDIT.txt deccd3de290fceb68bbdd57755ca96b7d7eb8b99be53e2fe16656647749ac9bd" \
        "LICENCE-BSD-2-Clause-Patent.txt a6fdb51d02b9dfe198e55534bb8047fe6d756eda117f4ff83de98f92c1952710" \
        "dis_1920x1080.yuv f8f8ae69c03d965e226f16415b49bc70b253a14bc715c7dd2a1226d41a676c88" \
        "dis_3840x2160.yuv 486dae17b9efc484183a10db65986f255e4bdc78ea4f7a8ea6a6afa14e219768" \
        "ref_1920x1080.yuv 3c6b8da1edb7b3f8e9b5350a53bb7e46366b2fde8e4c26fd06243261b0bd1caa" \
        "ref_3840x2160.yuv cc33c659808c46794bfe5f7b9dacbe4bc034c7d3ee76acf50b8805bf88bfc6ca"
      ;;
    *)
      return 1
      ;;
  esac
}

# Verify that all member files exist in the target directory with matching sha256.
verify_bundle_members() {
  local bundle_name="$1"
  local target_dir="$2"
  local members_list entry file expected actual path

  if ! members_list="$(get_bundle_members "${bundle_name}")"; then
    return 1
  fi

  while IFS= read -r entry; do
    [ -z "${entry}" ] && continue
    file="${entry% *}"
    expected="${entry##* }"
    path="${target_dir}/${file}"
    if [ ! -f "${path}" ]; then
      return 1
    fi
    actual="$(file_sha256 "${path}" | awk '{print $1}')"
    if [ "${actual}" != "${expected}" ]; then
      return 1
    fi
  done <<<"${members_list}"
  return 0
}

# Check tar archive for path traversal or absolute paths before extraction.
# Refuse every archive member that is not a regular file or a directory:
# a symlink or hard link could point the copy below outside the bundle.
# The verbose listing starts each line with the member's type ('-' file,
# 'd' directory) in GNU tar and bsdtar; a hard link to a file also starts
# with '-' in bsdtar, so its "link to" marker is refused as well.
check_member_types() {
  local archive="$1"
  local listing line

  if ! listing="$(tar -tvf "${archive}" 2>/dev/null)"; then
    echo "ERROR: failed to inspect archive contents: ${archive}" >&2
    echo "       GNU tar reads .tar.zst through the zstd program; is zstd on PATH?" >&2
    return 1
  fi

  while IFS= read -r line; do
    [ -z "${line}" ] && continue
    case "${line}" in
      [-d]*" link to "* | [-d]*" -> "* | [!-d]*)
        echo "ERROR: archive member is not a regular file or directory: ${line}" >&2
        return 1
        ;;
    esac
  done <<<"${listing}"
  return 0
}

check_path_traversal() {
  local archive="$1"
  local listing member

  if ! listing="$(tar -tf "${archive}" 2>/dev/null)"; then
    echo "ERROR: failed to inspect archive contents: ${archive}" >&2
    echo "       GNU tar reads .tar.zst through the zstd program; is zstd on PATH?" >&2
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

  if ! verify_bundle_members "${bundle_name}" "${src_dir}"; then
    echo "ERROR: ${bundle_name} member sha256 mismatch after extraction" >&2
    return 1
  fi

  rm -rf "${target_dir}"
  mkdir -p "${target_dir}"
  cp -a "${src_dir}/." "${target_dir}/"
  return 0
}

# Return the download URL for a bundle name.
bundle_url_for() {
  local bundle_name="$1"
  if [ -n "${BUNDLE_URL:-}" ]; then
    echo "${BUNDLE_URL}"
  elif [ -n "${BUNDLE_BASE_URL:-}" ]; then
    echo "${BUNDLE_BASE_URL}/${bundle_name}.tar.zst"
  else
    echo "https://github.com/VMAFx/vmafx/releases/download/fixtures-${bundle_name}/${bundle_name}.tar.zst"
  fi
}

# Fetch bundle archive and verify sha256 + safe archive structure.
fetch_and_check_archive() {
  local bundle_name="$1"
  local expected_sha="$2"
  local bundle_url="$3"
  local tmp_archive="$4"
  local actual_sha

  echo "fetch   bundle ${bundle_name}"
  if ! python3 -I "${DOWNLOAD}" "${bundle_url}" "${tmp_archive}"; then
    echo "ERROR: download failed for bundle ${bundle_name}" >&2
    return 1
  fi

  actual_sha="$(file_sha256 "${tmp_archive}" | awk '{print $1}')"
  if [ "${actual_sha}" != "${expected_sha}" ]; then
    echo "ERROR: ${bundle_name} archive sha256 mismatch (got ${actual_sha}, want ${expected_sha})" >&2
    return 1
  fi

  if ! check_path_traversal "${tmp_archive}" || ! check_member_types "${tmp_archive}"; then
    echo "ERROR: ${bundle_name} archive failed security validation" >&2
    return 1
  fi
  return 0
}

# Provision a fixture bundle into YUV_DIR/<bundle_name>.
provision_bundle() {
  local bundle_name="$1"
  local target_dir="${YUV_DIR}/${bundle_name}"
  local expected_sha bundle_url
  local tmp_archive stage_dir

  if ! expected_sha="$(get_bundle_sha "${bundle_name}")"; then
    echo "ERROR: ${bundle_name} is not a recognized bundle (supported: ${SUPPORTED_BUNDLES[*]})" >&2
    return 2
  fi

  if [ -d "${target_dir}" ] && verify_bundle_members "${bundle_name}" "${target_dir}"; then
    echo "ok      bundle ${bundle_name} (sha256 verified)"
    return 0
  fi

  if [ -d "${target_dir}" ]; then
    echo "stale   bundle ${bundle_name} — refetching"
    rm -rf "${target_dir}"
  fi

  # tar reads the .tar.zst through the zstd program; say so before any download.
  if ! command -v zstd >/dev/null 2>&1; then
    echo "ERROR: ${bundle_name} is a .tar.zst archive and zstd is not on PATH" >&2
    return 1
  fi

  mkdir -p "${YUV_DIR}"
  tmp_archive="$(mktemp "${YUV_DIR}/.${bundle_name}.tar.zst.XXXXXX")"
  stage_dir="$(mktemp -d "${YUV_DIR}/.${bundle_name}.stage.XXXXXX")"
  bundle_url="$(bundle_url_for "${bundle_name}")"

  if ! fetch_and_check_archive "${bundle_name}" "${expected_sha}" "${bundle_url}" "${tmp_archive}" ||
    ! extract_and_verify_bundle "${bundle_name}" "${tmp_archive}" "${stage_dir}" "${target_dir}"; then
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
        [ -n "${1#*=}" ] || {
          echo "ERROR: --bundle requires a bundle name" >&2
          return 2
        }
        PARSED_BUNDLE="${1#*=}"
        shift
        ;;
      --bundle)
        [ "$#" -ge 2 ] || {
          echo "ERROR: --bundle requires an argument" >&2
          return 2
        }
        [ -n "$2" ] || {
          echo "ERROR: --bundle requires a bundle name" >&2
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
    case "${PARSED_BUNDLE}" in
      pq-sollevante-v1 | hdr-ictcp-pixels-v1) ;;
      *)
        echo "ERROR: ${PARSED_BUNDLE} is not a recognized bundle (supported: ${SUPPORTED_BUNDLES[*]})" >&2
        return 2
        ;;
    esac
    if ! provision_bundle "${PARSED_BUNDLE}"; then
      return 1
    fi
    if [ "${#PARSED_FIXTURES[@]}" -eq 0 ]; then
      return 0
    fi
  fi

  listing="$(select_fixtures ${PARSED_FIXTURES[@]+"${PARSED_FIXTURES[@]}"})" || return 2
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
  case "${1:-}" in
    --print-bundle-sha)
      bundle="${2-pq-sollevante-v1}"
      if ! sha="$(get_bundle_sha "${bundle}")"; then
        echo "ERROR: ${bundle} is not a recognized bundle (supported: ${SUPPORTED_BUNDLES[*]})" >&2
        exit 2
      fi
      echo "${sha}"
      exit 0
      ;;
    --print-bundle-sha=*)
      bundle="${1#*=}"
      if ! sha="$(get_bundle_sha "${bundle}")"; then
        echo "ERROR: ${bundle} is not a recognized bundle (supported: ${SUPPORTED_BUNDLES[*]})" >&2
        exit 2
      fi
      echo "${sha}"
      exit 0
      ;;
  esac
  main "$@"
fi
