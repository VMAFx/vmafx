#!/usr/bin/env bash
# Enforce coverage thresholds from docs/principles.md §3.
# Usage: coverage-check.sh <gcovr-summary.json> <overall_min%> <critical_min%>
#
# Overall = unweighted average line coverage across every file in the gcovr
# summary (already filtered by gcovr to core/src/* and exclude test code,
# subprojects, /usr/*). Applies to upstream *and* fork-added code — we will
# be modifying upstream paths over time (SIMD fixes, refactors, bug fixes)
# and need a safety net.
#
# Security-critical = files under core/src/dnn/, plus opt.cpp and
# read_json_model.c (JSON + option parsing: user-supplied input paths).
# Files with zero gcovr data are skipped (no tests → nothing to assert).
#
# Why gcovr (not lcov): lcov sums hits/lines across every .gcno that names
# the same source — when foo.c is built into both libvmaf.so and N test
# binaries, lcov reports up to N+1× the real coverage and prints
# impossible >100% values (we observed dnn_api.c at 1176%). gcovr
# deduplicates by source path so the per-file numbers are honest.
# See ADR-0110.

set -euo pipefail

INFO="${1:?usage: coverage-check.sh <gcovr-summary.json> <overall_min%> <critical_min%>}"
OVERALL_MIN="${2:-70}"
CRITICAL_MIN="${3:-90}"

# Per-file critical-coverage overrides. Files listed here use the override
# instead of CRITICAL_MIN. Keep this list short and tied to ADRs so the
# rationale doesn't rot — every entry must cite the ADR that justifies the
# lower bar. See ADR-0114 for the EP-availability structural ceiling on
# the dnn/ort_backend.c + dnn/dnn_api.c entries.
declare -A PER_FILE_MIN=(
  # ADR-0114 baseline 78%; ADR-0922 +5pp ratchet target 83%.  Floor was
  # temporarily reset to 79 (PR #840) because ORT-operation-failure error
  # paths were structurally unreachable without error injection.  PR #844
  # added 8 ORT error-injection tests bringing coverage to 84%; floor
  # ratcheted back to 83 (safe slack against the 84% measured value).
  ["core/src/dnn/ort_backend.c"]=83
  # ADR-0114 baseline 78%; ratcheted +5pp to 83% by ADR-0922 (2026-05-31).
  ["core/src/dnn/dnn_api.c"]=83
  # core/src/dnn/tiny_extractor_template.h is a refactor template
  # of `static inline` helpers conditionally instantiated by per-extractor
  # callers. By design each new extractor uses a different subset of the
  # macro/helper menu (the whole point of the refactor is per-extractor
  # 30-LOC skeletons), so the unit suite covers only the helpers the
  # current extractors call. The lower bar reflects the structural ceiling;
  # adding tests just to inflate this number would be code-shaped padding,
  # not real correctness coverage. Distinct from opt.cpp / read_json_model.cpp
  # which parse user-supplied input and are properly security-critical.
  #
  # Ratcheted 10 → 75 on 2026-05-30 (ADR-0881): the live coverage on this
  # file climbed to 77.4 % once `feature_lpips.c`, `fastdvdnet_pre.c`,
  # `feature_mobilesal.c`, and `feature_transnet_v2.c` all landed, each
  # instantiating a different subset of the inline helpers. The 10 % cap
  # was set when only one consumer existed and ~90 % of the helpers were
  # uncalled. With four consumers the realistic floor is ~75 % (2.4 pp
  # slack against the 77.4 % measurement, mirroring the ~1.3 pp slack
  # ADR-0114 used for ort_backend.c). ADR-0881 also codifies the audit
  # rule: "tighten when actual exceeds override by >5 pp; keep at-cap
  # entries; remove when actual ≥ global 85 % floor."
  # ADR-0922 (2026-05-31) then applied its +5pp ratchet on top; the net
  # floor is 75 (the ADR-0881 value dominates the ADR-0922 +5pp increment).
  ["core/src/dnn/tiny_extractor_template.h"]=75
)

if ! command -v python3 >/dev/null; then
  echo "python3 not installed — cannot parse gcovr JSON" >&2
  exit 1
fi

if [ ! -f "$INFO" ]; then
  echo "coverage summary not found: $INFO" >&2
  exit 1
fi

# Pull the overall percent + per-file rows out of gcovr's --json-summary
# format. The schema:
#   { "line_percent": 73.4, "files": [
#       { "filename": "core/src/foo.c",
#         "line_percent": 81.2,
#         "line_total": 412, "line_covered": 335 }, ... ] }
# Note: avoid f-strings here. Python <3.12 forbids backslashes inside f-string
# expressions, and we are inside single-quoted bash so we cannot escape
# double quotes for the dict key. printf-style formatting sidesteps both.
read -r OVERALL <<<"$(python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
print("%.4f" % d.get("line_percent", 0))
' "$INFO")"

echo "Overall line coverage: ${OVERALL}% (min ${OVERALL_MIN}%)"

if awk -v c="$OVERALL" -v m="$OVERALL_MIN" 'BEGIN{exit !(c+0 < m+0)}'; then
  echo "FAIL: overall coverage ${OVERALL}% below minimum ${OVERALL_MIN}%" >&2
  exit 1
fi

# Per-file critical check. Print one line per critical file with the actual
# percentage; flag any below CRITICAL_MIN. Files with 0 lines (no tests yet)
# are surfaced but not enforced — easier to see the gap than to silently
# pass/fail.
PER_FILE="$(python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
for entry in d.get("files", []):
    fn = entry.get("filename", "")
    pct = entry.get("line_percent", 0.0)
    total = entry.get("line_total", 0)
    print(f"{fn}\t{pct:.4f}\t{total}")
' "$INFO")"

fail=0
while IFS=$'\t' read -r path pct total; do
  [ -z "$path" ] && continue
  case "$path" in
    *core/src/dnn/* | *core/src/opt.cpp | *core/src/read_json_model.cpp)
      if [ "$total" = "0" ] || awk -v c="$pct" 'BEGIN{exit !(c+0 == 0)}'; then
        echo "  critical (no tests yet — not enforced): $path — ${pct}%"
        continue
      fi
      # Normalise to a key that matches PER_FILE_MIN (gcovr emits paths
      # relative to the meson build dir; strip any leading ../). Default
      # to CRITICAL_MIN when no override exists.
      key="${path#../}"
      threshold="${PER_FILE_MIN[$key]:-$CRITICAL_MIN}"
      echo "  critical: $path — ${pct}% (min ${threshold}%)"
      if awk -v c="$pct" -v m="$threshold" 'BEGIN{exit !(c+0 < m+0)}'; then
        echo "    FAIL: security-critical file below ${threshold}%" >&2
        fail=1
      fi
      ;;
  esac
done <<<"$PER_FILE"

if [ "$fail" -ne 0 ]; then
  exit 1
fi

echo "PASS: coverage gate met (overall ≥${OVERALL_MIN}%, critical ≥${CRITICAL_MIN}% where tested)"
