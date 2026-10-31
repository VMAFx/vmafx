#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-vmafx-input-colorimetry.sh — an HDR-tagged input reaches VMAFx's
# conversion through the vmafx filter (ADR-2093, issue #2435).
#
# A copy of model/vmaf_v0.6.1.json gets a `conversion_target` (BT.2020 / PQ /
# ICtCp, limited range). The filter is run on 10-bit clips:
#   tagged     range tv, bt2020, smpte2084, bt2020nc on both inputs: exit 0, a
#              score (vmafx converted both pictures to the target)
#              positive: scores equal to CLI on same pair within 6 digits
#   diff_tags  differing ref (bt2020/smpte2084) and dist (bt709/bt709) tags:
#              exit 0, both converted independently by the engine to target
#   untagged   no colour tags: vmafx refuses with its "requires source
#              colorimetry" error naming the missing attributes, non-zero exit
#   plain      the unmodified model on the untagged clips: exit 0, a score
#              (behaviour without a conversion_target is unchanged)
#
# Needs an FFmpeg built with the series, linked against libvmafx built with
# -Denable_zimg=true, and python3. Without zimg the tagged cases are skipped
# and the script exits 77.
# Environment: FFMPEG (default ffmpeg), VMAF_MODEL_DIR (default: model/ of this
# repository), VMAF (default: vmaf).
#
# Exit codes: 0 pass, 1 fail, 77 skipped.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG="${FFMPEG:-ffmpeg}"
VMAF="${VMAF:-${VMAF_CLI:-vmaf}}"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
# shellcheck source=ffmpeg-patches/test/filter_check_lib.sh disable=SC1091
. "${HERE}/filter_check_lib.sh"

MODEL_DIR="${VMAF_MODEL_DIR:-${HERE}/../../model}"
command -v "${FFMPEG}" >/dev/null 2>&1 || skip "no ${FFMPEG}"
command -v python3 >/dev/null 2>&1 || skip "no python3"
has_filter vmafx || skip "${FFMPEG} has no vmafx filter"
[ -f "${MODEL_DIR}/vmaf_v0.6.1.json" ] || skip "no ${MODEL_DIR}/vmaf_v0.6.1.json"

python3 - "${MODEL_DIR}/vmaf_v0.6.1.json" "${WORK}/hdr_model.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
m["model_dict"]["conversion_target"] = {
    "colorspace": {"range": "limited", "primaries": "bt2020", "trc": "smpte2084", "matrix": "ictcp"}
}
json.dump(m, open(sys.argv[2], "w"))
PY

# Two 10-bit clips without colour tags: a reference and a noisier distorted one.
clip() { # $1 = out, $2 = noise filter (may be empty)
  local filt="format=yuv420p10le"
  if [ -n "$2" ]; then
    filt="format=yuv420p10le,$2,format=yuv420p10le"
  fi
  "${FFMPEG}" -hide_banner -loglevel error -y -f lavfi \
    -i "testsrc2=size=256x144:rate=5:duration=2,${filt}" \
    -f nut -c:v ffv1 "$1"
}
clip "${WORK}/ref.nut" ""
clip "${WORK}/dis.nut" "noise=alls=12:allf=t"

run() { # $1 = tag, $2 = model path, $3 = dist filter, $4 = ref filter
  local pre_d="${3:+,$3}"
  local pre_r="${4:+,$4}"
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info -i "${WORK}/dis.nut" -i "${WORK}/ref.nut" \
    -filter_complex "[0:v]null${pre_d}[d];[1:v]null${pre_r}[r];[d][r]vmafx=model=path=$2:log_fmt=json:log_path=${WORK}/$1.json:threads=1" \
    -f null - >"${WORK}/$1.log" 2>&1
}

zimg_skipped=0

# 1. Positive tagged case: both inputs tagged BT.2020 / PQ
TAGS="setparams=range=tv:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc"
rc=0
run tagged "${WORK}/hdr_model.json" "${TAGS}" "${TAGS}" || rc=$?
if grep -q "built without zimg" "${WORK}/tagged.log"; then
  echo "SKIP: tagged case: libvmafx was built without zimg"
  zimg_skipped=1
else
  expect "tagged: exit 0 (${rc})" test "${rc}" -eq 0
  expect "tagged: a VMAF score" grep -q "VMAF score" "${WORK}/tagged.log"
  expect "tagged: vmafx did not ask for colorimetry" lacks "requires source colorimetry" "${WORK}/tagged.log"

  if command -v "${VMAF}" >/dev/null 2>&1; then
    "${FFMPEG}" -hide_banner -loglevel error -y -i "${WORK}/ref.nut" -pix_fmt yuv420p10le "${WORK}/ref.yuv"
    "${FFMPEG}" -hide_banner -loglevel error -y -i "${WORK}/dis.nut" -pix_fmt yuv420p10le "${WORK}/dis.yuv"
    cli_status=0
    "${VMAF}" -w 256 -h 144 -b 10 -p 420 \
      -r "${WORK}/ref.yuv" -d "${WORK}/dis.yuv" \
      --model "path=${WORK}/hdr_model.json" \
      --color_range_ref limited --color_range_dist limited \
      --color_primaries_ref bt2020 --color_primaries_dist bt2020 \
      --color_trc_ref smpte2084 --color_trc_dist smpte2084 \
      --color_matrix_ref bt2020nc --color_matrix_dist bt2020nc \
      --threads 1 \
      --json -o "${WORK}/cli.json" >"${WORK}/cli.log" 2>&1 || cli_status=$?

    expect "CLI: exit 0 (${cli_status})" test "${cli_status}" -eq 0
    expect "CLI: cli.json generated" test -f "${WORK}/cli.json"
    expect "tagged: tagged.json generated" test -f "${WORK}/tagged.json"

    score_diff=$(
      python3 - "${WORK}/tagged.json" "${WORK}/cli.json" <<'PY'
import json, sys
ff = json.load(open(sys.argv[1]))
cli = json.load(open(sys.argv[2]))
s_ff = ff["pooled_metrics"]["vmaf"]["mean"]
s_cli = cli["pooled_metrics"]["vmaf"]["mean"]
diff = abs(s_ff - s_cli)
print(f"{diff:.8f}")
PY
    )
    expect "tagged score parity vs CLI within 6 digits ($score_diff)" \
      python3 -c "import sys; sys.exit(0 if float('$score_diff') < 1e-6 else 1)"
  else
    echo "FAIL: vmaf CLI executable not found on PATH" >&2
    fail=1
  fi
fi

# 2. Differing ref and dist tags: ref bt2020/smpte2084, dist bt709/bt709
TAGS_REF="setparams=range=tv:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc"
TAGS_DIST="setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt2020nc"
rc=0
run diff_tags "${WORK}/hdr_model.json" "${TAGS_DIST}" "${TAGS_REF}" || rc=$?
if [ "${zimg_skipped}" -eq 0 ]; then
  expect "differing tags: exit 0 (${rc})" test "${rc}" -eq 0
  expect "differing tags: a VMAF score" grep -q "VMAF score" "${WORK}/diff_tags.log"
  expect "differing tags: vmafx did not ask for colorimetry" lacks "requires source colorimetry" "${WORK}/diff_tags.log"

  if command -v "${VMAF}" >/dev/null 2>&1; then
    cli_status=0
    "${VMAF}" -w 256 -h 144 -b 10 -p 420 \
      -r "${WORK}/ref.yuv" -d "${WORK}/dis.yuv" \
      --model "path=${WORK}/hdr_model.json" \
      --color_range_ref limited --color_range_dist limited \
      --color_primaries_ref bt2020 --color_primaries_dist bt709 \
      --color_trc_ref smpte2084 --color_trc_dist bt709 \
      --color_matrix_ref bt2020nc --color_matrix_dist bt2020nc \
      --threads 1 \
      --json -o "${WORK}/cli_diff.json" >"${WORK}/cli_diff.log" 2>&1 || cli_status=$?

    expect "CLI differing tags: exit 0 (${cli_status})" test "${cli_status}" -eq 0
    diff_score_diff=$(
      python3 - "${WORK}/diff_tags.json" "${WORK}/cli_diff.json" <<'PY'
import json, sys
ff = json.load(open(sys.argv[1]))
cli = json.load(open(sys.argv[2]))
s_ff = ff["pooled_metrics"]["vmaf"]["mean"]
s_cli = cli["pooled_metrics"]["vmaf"]["mean"]
diff = abs(s_ff - s_cli)
print(f"{diff:.8f}")
PY
    )
    expect "differing tags score parity vs CLI within 6 digits ($diff_score_diff)" \
      python3 -c "import sys; sys.exit(0 if float('$diff_score_diff') < 1e-6 else 1)"

    expect "differing tags feature parity vs CLI and difference from tagged case" \
      python3 - "${WORK}/diff_tags.json" "${WORK}/cli_diff.json" "${WORK}/tagged.json" <<'PY'
import json, sys

ff = json.load(open(sys.argv[1]))
cli = json.load(open(sys.argv[2]))
tagged = json.load(open(sys.argv[3]))

ff_pooled = ff.get("pooled_metrics", {})
cli_pooled = cli.get("pooled_metrics", {})
tagged_pooled = tagged.get("pooled_metrics", {})

common_features = [k for k in sorted(ff_pooled.keys()) if k in cli_pooled]
if not common_features:
    print("FAIL: no common pooled features found between FFmpeg and CLI", file=sys.stderr)
    sys.exit(1)

has_nonzero = False
has_differing_from_tagged = False

print(f"{'Feature':38s} | {'FFmpeg':10s} | {'CLI':10s} | {'Diff':10s} | {'Tagged':10s}")
print("-" * 88)

for k in common_features:
    m_ff = ff_pooled[k]["mean"]
    m_cli = cli_pooled[k]["mean"]
    diff = abs(m_ff - m_cli)
    m_tagged = tagged_pooled.get(k, {}).get("mean", None)
    tagged_str = f"{m_tagged:.6f}" if m_tagged is not None else "N/A"
    print(f"{k:38s} | {m_ff:10.6f} | {m_cli:10.6f} | {diff:10.2e} | {tagged_str:10s}")

    if diff >= 1e-6:
        print(f"FAIL: feature {k} exceeds 1e-6 parity tolerance ({diff:.8e})", file=sys.stderr)
        sys.exit(1)

    if abs(m_ff) > 1e-6:
        has_nonzero = True

    if m_tagged is not None and abs(m_ff - m_tagged) > 1e-6:
        has_differing_from_tagged = True

if not has_nonzero:
    print("FAIL: no feature has nonzero mean", file=sys.stderr)
    sys.exit(1)

if not has_differing_from_tagged:
    print("FAIL: differing-tags features do not differ from tagged case", file=sys.stderr)
    sys.exit(1)
PY
  fi
fi

# 3. Untagged input with conversion_target: must refuse with descriptive error
rc=0
run untagged "${WORK}/hdr_model.json" "" "" || rc=$?
expect "untagged: non-zero exit (${rc})" test "${rc}" -ne 0
expect "untagged: vmafx names the missing colorimetry" \
  grep -q "requires source colorimetry" "${WORK}/untagged.log"
expect "untagged: no VMAF score" lacks "VMAF score" "${WORK}/untagged.log"

# 4. Untagged input without conversion_target: scores normally
rc=0
run plain "${MODEL_DIR}/vmaf_v0.6.1.json" "" "" || rc=$?
expect "no target, untagged: exit 0 (${rc})" test "${rc}" -eq 0
expect "no target, untagged: a VMAF score" grep -q "VMAF score" "${WORK}/plain.log"

if [ "${fail}" -ne 0 ]; then
  exit 1
fi

if [ "${zimg_skipped}" -ne 0 ]; then
  echo "SKIP: tagged cases skipped because libvmafx was built without zimg"
  exit 77
fi

exit 0
