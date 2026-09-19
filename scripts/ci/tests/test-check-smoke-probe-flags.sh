#!/usr/bin/env bash
# Test harness for scripts/ci/check-smoke-probe-flags.sh.
#
# Usage: bash scripts/ci/tests/test-check-smoke-probe-flags.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECK_SCRIPT="$SCRIPT_DIR/../check-smoke-probe-flags.sh"
ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"

if [[ ! -f "$CHECK_SCRIPT" ]]; then
  printf 'ERROR: %s not found\n' "$CHECK_SCRIPT" >&2
  exit 1
fi

TMPDIR_TESTS="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_TESTS"' EXIT

pass=0
fail=0

assert_eq() {
  local desc="$1"
  local expected="$2"
  local actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  PASS: %s\n' "$desc"
    pass=$((pass + 1))
  else
    printf '  FAIL: %s\n' "$desc" >&2
    printf '    expected: %s\n' "$expected" >&2
    printf '    actual:   %s\n' "$actual" >&2
    fail=$((fail + 1))
  fi
}

# A minimal but representative long_opts[] table: enough entries to
# exercise both a passing and a failing probe script against it.
write_mock_cli_parse() {
  local dir="$1"
  mkdir -p "$dir/core/tools"
  cat <<'MOCK_EOF' >"$dir/core/tools/cli_parse.cpp"
const struct option long_opts[] = {
    {.name = "reference", .has_arg = 1, .flag = nullptr, .val = 'r'},
    {.name = "distorted", .has_arg = 1, .flag = nullptr, .val = 'd'},
    {.name = "width", .has_arg = 1, .flag = nullptr, .val = 'w'},
    {.name = "height", .has_arg = 1, .flag = nullptr, .val = 'h'},
    {.name = "pixel_format", .has_arg = 1, .flag = nullptr, .val = 'p'},
    {.name = "model", .has_arg = 1, .flag = nullptr, .val = 'm'},
    {.name = "output", .has_arg = 1, .flag = nullptr, .val = 'o'},
    {.name = "backend", .has_arg = 1, .flag = nullptr, .val = ARG_BACKEND},
    {.name = "no_prediction", .has_arg = 0, .flag = nullptr, .val = 'n'},
    {.name = nullptr, .has_arg = 0, .flag = nullptr, .val = 0},
};
MOCK_EOF
}

write_mock_probe_script() {
  local dir="$1"
  local backend_flag_line="$2"
  local prediction_flag="$3"
  mkdir -p "$dir/dev/scripts"
  cat <<MOCK_EOF >"$dir/dev/scripts/smoke-probe-loop.sh"
#!/usr/bin/env bash
probe_backend() {
  local backend="\${1}"
  case "\${backend}" in
    cpu) backend_flag="" ;;
    cuda) ${backend_flag_line} ;;
  esac
  if vmaf \\
    --reference "\${REF_YUV}" \\
    --distorted "\${DIS_YUV}" \\
    --width "\${WIDTH}" \\
    --height "\${HEIGHT}" \\
    --pixel_format "\${PIXEL_FORMAT}" \\
    --model "path=\${VMAF_MODEL}" \\
    --output /dev/null \\
    \${backend_flag} \\
    ${prediction_flag} \\
    >"\${tmp_out}" 2>&1; then
    :
  fi
}
MOCK_EOF
}

echo "=== Test 1: Real tree passes ==="
out=""
exit_code=0
out=$(bash "$CHECK_SCRIPT" "$ROOT" 2>&1) || exit_code=$?
assert_eq "Real tree exits 0" "0" "$exit_code"
assert_eq "Real tree outputs PASS" "1" \
  "$(grep -cF 'PASS: every smoke-probe-loop.sh vmaf flag is registered' <<<"$out")"

echo "=== Test 2: Bare --cuda flag (not --backend=cuda) is caught ==="
mock_bad="$(mktemp -d -p "$TMPDIR_TESTS")"
write_mock_cli_parse "$mock_bad"
write_mock_probe_script "$mock_bad" 'backend_flag="--cuda"' "--no_prediction"

exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_bad" 2>&1) || exit_code=$?
assert_eq "Bare --cuda exits 1" "1" "$exit_code"
assert_eq "MISSING --cuda reported" "1" \
  "$(grep -cF 'MISSING (backend_flag case): --cuda is not registered' <<<"$out")"

echo "=== Test 3: --no_prediction_flags (not --no_prediction) is caught ==="
mock_bad2="$(mktemp -d -p "$TMPDIR_TESTS")"
write_mock_cli_parse "$mock_bad2"
write_mock_probe_script "$mock_bad2" 'backend_flag="--backend=cuda"' "--no_prediction_flags"

exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_bad2" 2>&1) || exit_code=$?
assert_eq "--no_prediction_flags exits 1" "1" "$exit_code"
assert_eq "MISSING --no_prediction_flags reported" "1" \
  "$(grep -cF 'MISSING: --no_prediction_flags is not registered' <<<"$out")"

echo "=== Test 4: Correct --backend=cuda + --no_prediction passes ==="
mock_good="$(mktemp -d -p "$TMPDIR_TESTS")"
write_mock_cli_parse "$mock_good"
write_mock_probe_script "$mock_good" 'backend_flag="--backend=cuda"' "--no_prediction"

exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_good" 2>&1) || exit_code=$?
assert_eq "Corrected mock tree exits 0" "0" "$exit_code"
assert_eq "Corrected mock tree outputs PASS" "1" \
  "$(grep -cF 'PASS: every smoke-probe-loop.sh vmaf flag is registered' <<<"$out")"

echo "=== Test 5: A comment mentioning a bad flag is not mistaken for literal use ==="
mock_comment="$(mktemp -d -p "$TMPDIR_TESTS")"
write_mock_cli_parse "$mock_comment"
mkdir -p "$mock_comment/dev/scripts"
cat <<'MOCK_EOF' >"$mock_comment/dev/scripts/smoke-probe-loop.sh"
#!/usr/bin/env bash
probe_backend() {
  local backend="${1}"
  # there is no bare --cuda/--sycl/--hip flag; use --backend=NAME instead
  case "${backend}" in
    cpu) backend_flag="" ;;
    cuda) backend_flag="--backend=cuda" ;;
  esac
  if vmaf \
    --reference "${REF_YUV}" \
    --distorted "${DIS_YUV}" \
    --width "${WIDTH}" \
    --height "${HEIGHT}" \
    --pixel_format "${PIXEL_FORMAT}" \
    --model "path=${VMAF_MODEL}" \
    --output /dev/null \
    ${backend_flag} \
    --no_prediction \
    >"${tmp_out}" 2>&1; then
    :
  fi
}
MOCK_EOF

exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_comment" 2>&1) || exit_code=$?
assert_eq "Comment-only mention does not fail the check" "0" "$exit_code"

echo "=== Test 6: Missing cli_parse.cpp triggers error ==="
mock_nocli="$(mktemp -d -p "$TMPDIR_TESTS")"
mkdir -p "$mock_nocli/dev/scripts"
: >"$mock_nocli/dev/scripts/smoke-probe-loop.sh"
exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_nocli" 2>&1) || exit_code=$?
assert_eq "Missing cli_parse.cpp exits 1" "1" "$exit_code"
assert_eq "Error message emitted" "1" "$(grep -cF 'ERROR: cli_parse.cpp not found' <<<"$out")"

echo ""
echo "=== Summary: $pass passed, $fail failed ==="
if [[ "$fail" -gt 0 ]]; then
  exit 1
fi
