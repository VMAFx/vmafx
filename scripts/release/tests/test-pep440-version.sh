#!/usr/bin/env bash
# Regression tests for the PEP 440 release-version conversion and its use in
# the supply-chain workflow's Python distribution filename matches.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# ADR-1201 release candidates are tagged `vX.Y.Z-rc.N`, but hatchling names the
# vmaf-mcp distributions after the PEP 440 normalized version
# (`vmaf_mcp-1.0.0rc1-py3-none-any.whl`, `vmaf_mcp-1.0.0rc1.tar.gz`). A glob
# built from the SemVer string matched nothing, so mcp-build failed and SBOM,
# signing, PyPI publication and release attachment were all skipped.

# The workflow assertions compare literal `${{ ... }}` / `$VAR` text, so single
# quotes are intended throughout.
# shellcheck disable=SC2016  # literal workflow text, never expanded here

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
CONVERT="$SCRIPT_DIR/../pep440-version.sh"
WORKFLOW="$REPO_ROOT/.github/workflows/supply-chain.yml"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0

ok() {
  printf 'PASS: %s\n' "$1"
  pass=$((pass + 1))
}

bad() {
  printf 'FAIL: %s\n' "$1" >&2
  fail=$((fail + 1))
}

# --- Converter: accepted shapes ----------------------------------------------

expect_converts() {
  local input="$1" expected="$2" output rc=0
  output="$("$CONVERT" "$input" 2>"$scratch/stderr")" || rc=$?
  if [[ "$rc" -eq 0 && "$output" == "$expected" && ! -s "$scratch/stderr" ]]; then
    ok "converts '$input' -> '$expected'"
  else
    bad "converts '$input': expected '$expected' (exit 0), got '$output' (exit $rc)"
  fi
}

expect_converts 1.0.0 1.0.0
expect_converts 1.0.0-rc.1 1.0.0rc1
expect_converts 1.2.3-rc.10 1.2.3rc10
# Boundaries: every component at zero, the smallest candidate number, and
# multi-digit components everywhere.
expect_converts 0.0.0 0.0.0
expect_converts 0.0.0-rc.0 0.0.0rc0
expect_converts 10.20.30-rc.99 10.20.30rc99

# --- Converter: everything else fails closed ---------------------------------

expect_rejected() {
  local description="$1" output rc=0
  shift
  output="$("$CONVERT" "$@" 2>/dev/null)" || rc=$?
  if [[ "$rc" -eq 64 && -z "$output" ]]; then
    ok "rejects $description"
  else
    bad "rejects $description: expected exit 64 and no output, got exit $rc '$output'"
  fi
}

expect_rejected "leading-zero candidate number '1.0.0-rc.01'" 1.0.0-rc.01
expect_rejected "non-rc prerelease '1.0.0-beta'" 1.0.0-beta
expect_rejected "tag spelling 'v1.0.0'" v1.0.0
expect_rejected "tag spelling 'v1.0.0-rc.1'" v1.0.0-rc.1
expect_rejected "two-component version '1.0'" 1.0
expect_rejected "four-component version '1.0.0.0'" 1.0.0.0
expect_rejected "leading-zero component '01.0.0'" 01.0.0
expect_rejected "bare rc label '1.0.0-rc'" 1.0.0-rc
expect_rejected "dotted candidate '1.0.0-rc.1.2'" 1.0.0-rc.1.2
expect_rejected "already-normalized input '1.0.0rc1'" 1.0.0rc1
expect_rejected "uppercase label '1.0.0-RC.1'" 1.0.0-RC.1
expect_rejected "trailing newline" $'1.0.0\n'
expect_rejected "embedded newline" $'1.0.0\n1.0.0'
expect_rejected "leading space" ' 1.0.0'
expect_rejected "empty version" ''
expect_rejected "no argument"
expect_rejected "two arguments" 1.0.0 1.0.1

# --- Converter agrees with packaging, when it is importable ------------------
#
# The workflow must not depend on packaging (validate-release installs nothing),
# but where the reference implementation is present it is the oracle.
if python3 -c 'import packaging.version' >/dev/null 2>&1; then
  for input in 1.0.0 1.0.0-rc.1 1.2.3-rc.10 0.0.0-rc.0 10.20.30-rc.99; do
    expected="$(python3 -c 'import sys; from packaging.version import Version; print(Version(sys.argv[1]))' "$input")"
    if [[ "$("$CONVERT" "$input")" == "$expected" ]]; then
      ok "matches packaging.version.Version for '$input'"
    else
      bad "differs from packaging.version.Version for '$input' ($expected)"
    fi
  done
else
  printf 'SKIP: packaging is not importable; oracle comparison not run\n'
fi

# --- supply-chain.yml derives and exports the PEP 440 version ---------------

active="$scratch/supply-chain.active"
# Comments cannot satisfy or break any assertion below.
sed -E 's/(^|[[:space:]])#.*$//' "$WORKFLOW" >"$active"

if grep -Fqx '      pep440_version: ${{ steps.release.outputs.pep440_version }}' "$active"; then
  ok "validate-release exports pep440_version"
else
  bad "validate-release does not export pep440_version from the release step"
fi
derive='          pep440_version="$(scripts/release/pep440-version.sh "${RELEASE_TAG#v}")"'
if grep -Fqx -- "$derive" "$active" &&
  grep -Fqx -- '            echo "pep440_version=$pep440_version"' "$active" &&
  grep -Fqx -- '          } >> "$GITHUB_OUTPUT"' "$active"; then
  ok "validate-release derives pep440_version with pep440-version.sh"
else
  bad "validate-release does not derive pep440_version with pep440-version.sh"
fi
env_lines="$(grep -E '^[[:space:]]+VMAFX_PEP440_VERSION:' "$active" || true)"
if [[ -n "$env_lines" ]] &&
  ! grep -Fvx '          VMAFX_PEP440_VERSION: ${{ needs.validate-release.outputs.pep440_version }}' \
    <<<"$env_lines" >/dev/null; then
  ok "every VMAFX_PEP440_VERSION binding reads needs.validate-release.outputs.pep440_version"
else
  bad "a VMAFX_PEP440_VERSION binding is missing or reads another source"
fi

# --- Filename matches use the PEP 440 version -------------------------------
#
# Every version-bound wheel/sdist glob is resolved against the filenames that
# hatchling produces, next to the neighbouring version's files as decoys. A line
# must have one strict shape; its pieces are parsed out and the glob is rebuilt
# here, so nothing from the workflow is ever executed.

glob_shape='^[[:space:]]*(wheels|sdists)=\((mcp-dist|dist)/vmaf_mcp-"\$([A-Z0-9_]+)"(-\*\.whl|\.tar\.gz)\)$'

# Print how many fixture files one parsed glob selects.
count_matches() {
  local fixture="$1" dir="$2" value="$3" suffix="$4" path existing=0
  local -a matched=()
  shopt -s nullglob
  if [[ "$suffix" == '-*.whl' ]]; then
    matched=("$fixture/$dir/vmaf_mcp-$value"-*.whl)
  else
    matched=("$fixture/$dir/vmaf_mcp-$value.tar.gz")
  fi
  shopt -u nullglob
  # An sdist pattern has no wildcard, so nullglob keeps the literal path even
  # when no such file exists. Count only entries that name a real file.
  for path in "${matched[@]}"; do
    if [[ -f "$path" ]]; then
      existing=$((existing + 1))
    fi
  done
  printf '%s' "$existing"
}

check_globs() {
  local workflow="$1" semver="$2" pep440="$3" decoy="$4"
  local fixture="$scratch/fixture-$semver" line kind dir var suffix value count
  local lines=0 errors=0
  rm -rf "$fixture"
  mkdir -p "$fixture/mcp-dist" "$fixture/dist"
  for dir in mcp-dist dist; do
    for value in "$pep440" "$decoy"; do
      : >"$fixture/$dir/vmaf_mcp-$value-py3-none-any.whl"
      : >"$fixture/$dir/vmaf_mcp-$value.tar.gz"
    done
  done
  while IFS= read -r line; do
    lines=$((lines + 1))
    if [[ ! "$line" =~ $glob_shape ]]; then
      printf '  unparseable version-bound glob: %s\n' "$line" >&2
      errors=$((errors + 1))
      continue
    fi
    kind="${BASH_REMATCH[1]}"
    dir="${BASH_REMATCH[2]}"
    var="${BASH_REMATCH[3]}"
    suffix="${BASH_REMATCH[4]}"
    case "$kind:$suffix:$var" in
      'wheels:-*.whl:VMAFX_VERSION' | 'sdists:.tar.gz:VMAFX_VERSION') value="$semver" ;;
      'wheels:-*.whl:VMAFX_PEP440_VERSION' | 'sdists:.tar.gz:VMAFX_PEP440_VERSION') value="$pep440" ;;
      *)
        printf '  unexpected kind/suffix/variable in glob: %s\n' "$line" >&2
        errors=$((errors + 1))
        continue
        ;;
    esac
    count="$(count_matches "$fixture" "$dir" "$value" "$suffix")"
    if [[ "$count" -ne 1 ]]; then
      printf '  %s matched %s file(s) for %s: %s\n' "$kind" "$count" "$semver" "$line" >&2
      errors=$((errors + 1))
    fi
  done < <(grep -E '^[[:space:]]*(wheels|sdists)=\(.*vmaf_mcp-.*\$' "$workflow" || true)
  [[ "$lines" -gt 0 && "$errors" -eq 0 ]]
}

expect_globs() {
  local description="$1"
  shift
  if check_globs "$@"; then
    ok "$description"
  else
    bad "$description"
  fi
}

expect_globs "rc wheel/sdist globs match hatchling's 1.0.0rc1 filenames" \
  "$active" 1.0.0-rc.1 1.0.0rc1 1.0.0
expect_globs "multi-digit rc globs match hatchling's 1.2.3rc10 filenames" \
  "$active" 1.2.3-rc.10 1.2.3rc10 1.2.3rc1
expect_globs "final wheel/sdist globs still match 1.0.0 and not 1.0.0rc1" \
  "$active" 1.0.0 1.0.0 1.0.0rc1

pypi_urls="$(grep -F 'pypi.org/pypi/vmaf-mcp/' "$active" || true)"
if [[ -n "$pypi_urls" ]] &&
  ! grep -Fv '"https://pypi.org/pypi/vmaf-mcp/$VMAFX_PEP440_VERSION/json"' \
    <<<"$pypi_urls" >/dev/null; then
  ok "PyPI release queries name the PEP 440 version"
else
  bad "a PyPI release query does not name the PEP 440 version"
fi

# The version-bound sdist "glob" has no wildcard, so the workflow must prove
# the file exists; otherwise a wrong sdist name still counts as one match.
sdist_lines="$(grep -cE '^[[:space:]]*sdists=\(.*vmaf_mcp-.*\$' "$active" || true)"
exists_checks="$(grep -cF '! -f "${sdists[0]}"' "$active" || true)"
if [[ "$sdist_lines" -gt 0 && "$exists_checks" -eq "$sdist_lines" ]]; then
  ok "every version-bound sdist match requires the file to exist ($sdist_lines sites)"
else
  bad "version-bound sdist matches: $sdist_lines, existence checks: $exists_checks"
fi

# The gate has teeth: restoring a SemVer-built glob in any one job is caught.
expect_regression_caught() {
  local description="$1" expression="$2" regressed="$scratch/supply-chain.regressed"
  sed "$expression" "$active" >"$regressed"
  if cmp -s "$active" "$regressed"; then
    bad "regression fixture mutation did not apply: $description"
  elif check_globs "$regressed" 1.0.0-rc.1 1.0.0rc1 1.0.0 2>/dev/null; then
    bad "a SemVer-built $description unexpectedly matched the rc distributions"
  else
    ok "rejects a workflow whose $description is built from the SemVer version"
  fi
}

expect_regression_caught "wheel glob" \
  '0,/vmaf_mcp-"\$VMAFX_PEP440_VERSION"-\*\.whl/s//vmaf_mcp-"$VMAFX_VERSION"-*.whl/'
expect_regression_caught "sdist match" \
  '0,/vmaf_mcp-"\$VMAFX_PEP440_VERSION"\.tar\.gz/s//vmaf_mcp-"$VMAFX_VERSION".tar.gz/'

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
