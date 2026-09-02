#!/usr/bin/env bash
# Regression tests for the fragment-owned release rollover contract.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROLLOVER="$SCRIPT_DIR/../rollover-changelog-fragments.sh"
CONCAT="$SCRIPT_DIR/../concat-changelog-fragments.sh"

pass=0
fail=0
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

check() {
  local description="$1"
  local result="$2"
  if [[ "$result" == pass ]]; then
    printf 'PASS: %s\n' "$description"
    pass=$((pass + 1))
  else
    printf 'FAIL: %s\n' "$description" >&2
    fail=$((fail + 1))
  fi
}

tree_hash() {
  local root="$1"
  find "$root" -type f -print0 |
    LC_ALL=C sort -z |
    xargs -0 sha256sum |
    sha256sum |
    cut -d' ' -f1
}

fixture() {
  local root="$1"
  local manifest_version="${2:-3.2.1}"
  local marker_version="${3:-3.2.1}"
  mkdir -p "$root/scripts/release" "$root/changelog.d/added" \
    "$root/changelog.d/fixed"
  cp "$ROLLOVER" "$root/scripts/release/"
  cp "$CONCAT" "$root/scripts/release/"
  chmod +x "$root/scripts/release/"*.sh
  printf '{".":"%s"}\n' "$manifest_version" >"$root/.release-please-manifest.json"
  printf '%s\n' \
    '{' \
    '  "bootstrap-sha": "0000000000000000000000000000000000000000",' \
    '  "packages": {' \
    '    ".": {' \
    '      "release-as": "3.2.1",' \
    '      "skip-changelog": true,' \
    '      "extra-files": [{"type":"generic","path":"version-marker.txt"}]' \
    '    }' \
    '  }' \
    '}' >"$root/release-please-config.json"
  printf 'version=%s # x-release-please-version\n' "$marker_version" \
    >"$root/version-marker.txt"
  printf '%s\n' \
    '# Changelog' \
    '' \
    '## [Unreleased]' \
    '' \
    'legacy entry' \
    '' \
    '### Added' \
    '' \
    '- added entry' \
    '' \
    '### Fixed' \
    '' \
    '- fixed entry' \
    '' \
    '## [3.2.0] - 2026-08-01' \
    '' \
    '- prior release' >"$root/CHANGELOG.md"
  printf 'legacy entry\n\n' >"$root/changelog.d/_pre_fragment_legacy.md"
  printf '%s\n' '- added entry' >"$root/changelog.d/added/add.md"
  printf '%s\n' '- fixed entry' >"$root/changelog.d/fixed/fix.md"
  VMAFX_REPO_ROOT="$root" "$root/scripts/release/concat-changelog-fragments.sh" \
    --write >/dev/null 2>&1
}

printf '\n=== release fragment rollover tests ===\n\n'

# T1: Happy path preserves the prior release, versions the rendered body,
# consumes all active sources, writes a receipt, and leaves concat --check green.
happy="$scratch/happy"
fixture "$happy"
prior_hash="$(awk '/^## \[3.2.0\]/{found=1} found{print}' "$happy/CHANGELOG.md" | sha256sum | cut -d' ' -f1)"
if VMAFX_REPO_ROOT="$happy" "$happy/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-31 >/dev/null; then
  check 'happy rollover exits zero' pass
else
  check 'happy rollover exits zero' fail
fi
if [[ "$(grep -c '^## \[3.2.1\] - 2026-08-31$' "$happy/CHANGELOG.md")" -eq 1 ]] &&
  [[ "$(awk '/^## \[3.2.0\]/{found=1} found{print}' "$happy/CHANGELOG.md" | sha256sum | cut -d' ' -f1)" == "$prior_hash" ]]; then
  check 'release heading is unique and prior tail is byte-identical' pass
else
  check 'release heading is unique and prior tail is byte-identical' fail
fi
if [[ ! -e "$happy/changelog.d/_pre_fragment_legacy.md" ]] &&
  [[ "$(find "$happy/changelog.d/added" "$happy/changelog.d/fixed" -type f | wc -l)" -eq 0 ]] &&
  jq -e '(has("bootstrap-sha") | not) and
    (.packages["."] | has("release-as") | not)' \
    "$happy/release-please-config.json" >/dev/null &&
  jq -e '.version == "3.2.1" and .source_count == 3' \
    "$happy/changelog.d/releases/3.2.1.json" >/dev/null &&
  VMAFX_REPO_ROOT="$happy" "$happy/scripts/release/concat-changelog-fragments.sh" --check; then
  check 'sources are consumed, receipt is exact, and post-cut renderer is clean' pass
else
  check 'sources are consumed, receipt is exact, and post-cut renderer is clean' fail
fi

# T2: Exact rerun is an idempotent no-op.
before="$(tree_hash "$happy")"
if VMAFX_REPO_ROOT="$happy" "$happy/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-31 >/dev/null &&
  [[ "$(tree_hash "$happy")" == "$before" ]]; then
  check 'second exact invocation is a no-op' pass
else
  check 'second exact invocation is a no-op' fail
fi

# T3: Invalid CLI inputs fail with EX_USAGE and do not mutate the tree.
invalid="$scratch/invalid"
fixture "$invalid"
before="$(tree_hash "$invalid")"
rc=0
VMAFX_REPO_ROOT="$invalid" "$invalid/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2 --date 2026-08-31 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 64 && "$(tree_hash "$invalid")" == "$before" ]]; then
  check 'invalid SemVer is rejected without mutation' pass
else
  check 'invalid SemVer is rejected without mutation' fail
fi

rc=0
VMAFX_REPO_ROOT="$invalid" "$invalid/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-02-30 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 64 && "$(tree_hash "$invalid")" == "$before" ]]; then
  check 'invalid calendar date is rejected without mutation' pass
else
  check 'invalid calendar date is rejected without mutation' fail
fi

# T4: Manifest/marker mismatch fails before mutation.
mismatch="$scratch/mismatch"
fixture "$mismatch" 3.2.0 3.2.1
before="$(tree_hash "$mismatch")"
rc=0
VMAFX_REPO_ROOT="$mismatch" "$mismatch/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-31 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -ne 0 && "$(tree_hash "$mismatch")" == "$before" ]]; then
  check 'manifest mismatch is rejected without mutation' pass
else
  check 'manifest mismatch is rejected without mutation' fail
fi

# T5: Renderer drift fails before mutation.
drift="$scratch/drift"
fixture "$drift"
printf '%s\n' '- unrendered late fragment' >"$drift/changelog.d/fixed/late.md"
before="$(tree_hash "$drift")"
rc=0
VMAFX_REPO_ROOT="$drift" "$drift/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-31 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -ne 0 && "$(tree_hash "$drift")" == "$before" ]]; then
  check 'renderer drift is rejected without mutation' pass
else
  check 'renderer drift is rejected without mutation' fail
fi

# T6: A stale release-as override for another version fails before mutation.
wrong_release_as="$scratch/wrong-release-as"
fixture "$wrong_release_as"
jq '.packages["."]."release-as" = "3.2.0"' \
  "$wrong_release_as/release-please-config.json" >"$wrong_release_as/config.tmp"
mv "$wrong_release_as/config.tmp" "$wrong_release_as/release-please-config.json"
before="$(tree_hash "$wrong_release_as")"
rc=0
VMAFX_REPO_ROOT="$wrong_release_as" \
  "$wrong_release_as/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-31 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -ne 0 && "$(tree_hash "$wrong_release_as")" == "$before" ]]; then
  check 'mismatched release-as is rejected without mutation' pass
else
  check 'mismatched release-as is rejected without mutation' fail
fi

# T7: Duplicate target heading fails before mutation.
duplicate="$scratch/duplicate"
fixture "$duplicate"
printf '\n## [3.2.1] - 2026-08-30\n' >>"$duplicate/CHANGELOG.md"
before="$(tree_hash "$duplicate")"
rc=0
VMAFX_REPO_ROOT="$duplicate" "$duplicate/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-31 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -ne 0 && "$(tree_hash "$duplicate")" == "$before" ]]; then
  check 'duplicate target release is rejected without mutation' pass
else
  check 'duplicate target release is rejected without mutation' fail
fi

# T8: An empty active release is refused.
empty="$scratch/empty"
fixture "$empty"
rm -f "$empty/changelog.d/_pre_fragment_legacy.md" \
  "$empty/changelog.d/added/add.md" "$empty/changelog.d/fixed/fix.md"
printf '%s\n' '# Changelog' '' '## [Unreleased]' '' '## [3.2.0] - 2026-08-01' \
  '' '- prior release' >"$empty/CHANGELOG.md"
before="$(tree_hash "$empty")"
rc=0
VMAFX_REPO_ROOT="$empty" "$empty/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-31 >/dev/null 2>&1 || rc=$?
if [[ "$rc" -ne 0 && "$(tree_hash "$empty")" == "$before" ]]; then
  check 'empty release is rejected without mutation' pass
else
  check 'empty release is rejected without mutation' fail
fi

printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
