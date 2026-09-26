#!/usr/bin/env bash
# Regression tests for the fragment-owned release rollover contract.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROLLOVER="$SCRIPT_DIR/../rollover-changelog-fragments.sh"
VERIFY="$SCRIPT_DIR/../verify-release-version.sh"
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
  local release_as="${4:-3.2.1}"
  mkdir -p "$root/scripts/release" "$root/changelog.d/added" \
    "$root/changelog.d/fixed"
  cp "$ROLLOVER" "$root/scripts/release/"
  cp "$CONCAT" "$root/scripts/release/"
  chmod +x "$root/scripts/release/"*.sh
  printf '{".":"%s"}\n' "$manifest_version" >"$root/.release-please-manifest.json"
  printf '%s\n' \
    '{' \
    '  "_comment_bootstrap_sha": "one-shot, ADR-1151",' \
    '  "bootstrap-sha": "0000000000000000000000000000000000000000",' \
    '  "packages": {' \
    '    ".": {' \
    '      "_comment_release_as": "one-shot, ADR-1151",' \
    "      \"release-as\": \"$release_as\"," \
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
    (has("_comment_bootstrap_sha") | not) and
    (.packages["."] | has("release-as") | not) and
    (.packages["."] | has("_comment_release_as") | not)' \
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

# T12: A body longer than --archive-over moves to docs/changelog-archive/ and
# CHANGELOG.md keeps a per-section index. The first release renders ~27,500
# lines from ~1,660 fragments; pasting that inline makes CHANGELOG.md unusable.
big="$scratch/archived"
fixture "$big"
for i in $(seq 1 60); do
  printf '%s\n' "- fixed entry $i" >"$big/changelog.d/fixed/fix$i.md"
done
VMAFX_REPO_ROOT="$big" "$big/scripts/release/concat-changelog-fragments.sh" \
  --write >/dev/null 2>&1
if VMAFX_REPO_ROOT="$big" "$big/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-02 --archive-over 20 >/dev/null 2>&1; then
  archive="$big/docs/changelog-archive/3.2.1.md"
  # The index replaces the body, the archive holds every entry, and the count
  # in the index is the number actually archived.
  if [[ -f "$archive" ]] &&
    grep -q '^## \[3.2.1\] - 2026-08-02$' "$big/CHANGELOG.md" &&
    grep -q 'docs/changelog-archive/3.2.1.md' "$big/CHANGELOG.md" &&
    # The fixture already ships one "- fixed entry", so 60 more makes 61.
    grep -q '^| Fixed | 61 |$' "$big/CHANGELOG.md" &&
    [[ "$(grep -c '^- fixed entry' "$archive")" -eq 61 ]] &&
    [[ "$(grep -c '^- fixed entry' "$big/CHANGELOG.md")" -eq 0 ]] &&
    grep -q '^## \[3.2.0\] - 2026-08-01$' "$big/CHANGELOG.md"; then
    check "long body is archived and indexed, prior release intact" pass
  else
    check "long body is archived and indexed, prior release intact" fail
  fi
else
  check "long body is archived and indexed, prior release intact" fail
fi

# T13: Under the threshold the body still lands inline, so ordinary releases
# are unaffected by the archive path.
small="$scratch/inline"
fixture "$small"
if VMAFX_REPO_ROOT="$small" "$small/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-02 --archive-over 400 >/dev/null 2>&1; then
  if [[ ! -d "$small/docs/changelog-archive" ]] &&
    grep -q '^- fixed entry$' "$small/CHANGELOG.md"; then
    check "short body stays inline and writes no archive" pass
  else
    check "short body stays inline and writes no archive" fail
  fi
else
  check "short body stays inline and writes no archive" fail
fi

# T14: A release candidate is cut like a release (ADR-1201). The tag-time
# verifier demands the heading, receipt and retired one-shot fields for
# v1.0.0-rc.1 too, so the rollover must accept the same narrow -rc.N shape and
# read an rc marker as the full version, not as its 1.0.0 prefix.
rc_cut="$scratch/rc-cut"
fixture "$rc_cut" 1.0.0-rc.1 1.0.0-rc.1 1.0.0-rc.1
if VMAFX_REPO_ROOT="$rc_cut" "$rc_cut/scripts/release/rollover-changelog-fragments.sh" \
  --version 1.0.0-rc.1 --date 2026-09-26 >/dev/null 2>&1 &&
  [[ "$(grep -Ec '^## \[1\.0\.0-rc\.1\] - 2026-09-26$' "$rc_cut/CHANGELOG.md")" -eq 1 ]] &&
  jq -e '.version == "1.0.0-rc.1" and .source_count == 3' \
    "$rc_cut/changelog.d/releases/1.0.0-rc.1.json" >/dev/null &&
  jq -e '(has("bootstrap-sha") | not) and (.packages["."] | has("release-as") | not)' \
    "$rc_cut/release-please-config.json" >/dev/null &&
  [[ "$(find "$rc_cut/changelog.d/added" "$rc_cut/changelog.d/fixed" -type f | wc -l)" -eq 0 ]]; then
  check 'release candidate is cut with its own heading, receipt and retired fields' pass
else
  check 'release candidate is cut with its own heading, receipt and retired fields' fail
fi
# The cut is what the tag-time verifier checks; the fixture has no .git, so the
# verifier skips only its tag-points-at-HEAD step.
if VMAFX_REPO_ROOT="$rc_cut" "$VERIFY" v1.0.0-rc.1 >/dev/null 2>&1; then
  check 'tag-time verifier accepts the release-candidate cut' pass
else
  check 'tag-time verifier accepts the release-candidate cut' fail
fi

# T15: Every prerelease shape the verifier refuses is refused here too, as a
# usage error and without touching the tree.
rc_shapes="$scratch/rc-shapes"
fixture "$rc_shapes" 1.0.0-rc.1 1.0.0-rc.1 1.0.0-rc.1
before="$(tree_hash "$rc_shapes")"
shapes_ok=pass
for shape in 1.0.0-beta 1.0.0-rc 1.0.0-rc.01 1.0.0-rc.1.2 1.0.0-RC.1 1.0.0-rc.-1; do
  rc=0
  VMAFX_REPO_ROOT="$rc_shapes" "$rc_shapes/scripts/release/rollover-changelog-fragments.sh" \
    --version "$shape" --date 2026-09-26 >/dev/null 2>&1 || rc=$?
  [[ "$rc" -eq 64 ]] || shapes_ok=fail
done
[[ "$(tree_hash "$rc_shapes")" == "$before" ]] || shapes_ok=fail
check 'non-rc prerelease shapes are rejected without mutation' "$shapes_ok"

# T16: Boundary between the triple and its candidate: a plain 1.0.0 marker does
# not satisfy 1.0.0-rc.1, and an rc manifest does not satisfy --version 1.0.0.
rc_marker="$scratch/rc-marker"
fixture "$rc_marker" 1.0.0-rc.1 1.0.0 1.0.0-rc.1
before="$(tree_hash "$rc_marker")"
rc_a=0
VMAFX_REPO_ROOT="$rc_marker" "$rc_marker/scripts/release/rollover-changelog-fragments.sh" \
  --version 1.0.0-rc.1 --date 2026-09-26 >/dev/null 2>&1 || rc_a=$?
rc_manifest="$scratch/rc-manifest"
fixture "$rc_manifest" 1.0.0-rc.1 1.0.0-rc.1 1.0.0-rc.1
before_manifest="$(tree_hash "$rc_manifest")"
rc_b=0
VMAFX_REPO_ROOT="$rc_manifest" "$rc_manifest/scripts/release/rollover-changelog-fragments.sh" \
  --version 1.0.0 --date 2026-09-26 >/dev/null 2>&1 || rc_b=$?
boundary_ok=pass
[[ "$rc_a" -ne 0 && "$(tree_hash "$rc_marker")" == "$before" ]] || boundary_ok=fail
[[ "$rc_b" -ne 0 && "$(tree_hash "$rc_manifest")" == "$before_manifest" ]] || boundary_ok=fail
check 'triple and rc versions never satisfy each other' "$boundary_ok"

# T17: A first release has no older section under Unreleased. The archived
# index must still end CHANGELOG.md with exactly one newline, or the cut commit
# fails end-of-file-fixer.
first="$scratch/first-release"
fixture "$first"
printf '%s\n' '# Changelog' '' '## [Unreleased]' '' >"$first/CHANGELOG.md"
VMAFX_REPO_ROOT="$first" "$first/scripts/release/concat-changelog-fragments.sh" \
  --write >/dev/null 2>&1
eof_ok=fail
if VMAFX_REPO_ROOT="$first" "$first/scripts/release/rollover-changelog-fragments.sh" \
  --version 3.2.1 --date 2026-08-02 --archive-over 1 >/dev/null 2>&1 &&
  [[ "$(tail -c 1 "$first/CHANGELOG.md" | od -An -c | tr -d ' ')" == '\n' ]] &&
  [[ "$(tail -c 2 "$first/CHANGELOG.md" | od -An -c | tr -d ' ')" != '\n\n' ]]; then
  eof_ok=pass
fi
check 'first-release archived cut ends CHANGELOG.md with one newline' "$eof_ok"

printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
