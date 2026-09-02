#!/usr/bin/env bash
# PostToolUse hook: warn when a numerical-code file was edited, reminding the agent
# that snapshot regeneration may be required via /regen-snapshots.
# Does not block; informational only.
set -euo pipefail

file="${CLAUDE_TOOL_INPUT_file_path:-}"
if [[ -z "$file" ]] && command -v jq >/dev/null 2>&1; then
  input=$(cat 2>/dev/null || true)
  [[ -n "$input" ]] && file=$(printf '%s' "$input" | jq -r '.tool_input.file_path // empty' 2>/dev/null || true)
fi
[[ -z "$file" ]] && exit 0

# In bash case-globs, '*' matches '/', so '*/core/src/feature/*.c'
# also catches files in subdirectories (x86/, arm64/, cuda/, sycl/).
case "$file" in
  */core/src/feature/*.c | */core/src/feature/*.h | \
    */core/src/feature/*.cu | */core/src/feature/*.cuh | \
    */core/src/feature/*.cpp | */core/src/feature/*.hpp)
    cat >&2 <<EOF
NOTICE: edited feature extractor code: $file

If this changes numerical output (even slightly), test snapshots under
'testdata/scores_cpu_*.json' and 'testdata/netflix_benchmark_results.json' may need
regeneration. Run '/regen-snapshots' and include a justification in the commit message.

If this change is expected to be bit-exact (pure perf), run '/cross-backend-diff' to
confirm ULP == 0 before proceeding.

Netflix golden-data tests (python/test/*.py assertAlmostEqual) must NEVER be modified.
EOF
    ;;
  */python/test/quality_runner_test.py | \
    */python/test/vmafexec_test.py | \
    */python/test/vmafexec_feature_extractor_test.py | \
    */python/test/feature_extractor_test.py | \
    */python/test/result_test.py)
    cat >&2 <<EOF
ERROR: edit detected to a Netflix golden-data test file: $file

These tests contain the canonical ground-truth assertions for VMAF numerical correctness
(see docs/principles.md §3.1 and CLAUDE.md §8). They are preserved verbatim. If your
change is strictly additive (new test cases in separate functions), that is allowed, but
existing 'assertAlmostEqual' values must not be modified. Review your diff before commit.
EOF
    ;;
esac

exit 0
