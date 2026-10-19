#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/githooks/pre-commit.sh — native pre-commit hook (opt-in).
#
# Native bash alternative to the pre-commit-framework hook
# (`pre-commit install --install-hooks`). Same intent — format and
# auto-fix staged files before the commit — but executes the
# formatters directly via shelling out, skipping the per-hook venv
# wrap that the pre-commit framework imposes (~3 s/hook startup).
# On a typical 4-formatter touch the native path runs in ~0.4 s,
# vs ~3 s for the framework path.
#
# Install with `VMAFX_NATIVE_HOOKS=1 make install-hooks` (or by
# running scripts/githooks/install.sh directly with the env var set).
# Without the env var, `make install-hooks` continues to install the
# pre-commit-framework hook as before — this script is opt-in and
# does not displace the default flow.
#
# CI continues to use the pre-commit framework against the full tree
# (`.pre-commit-config.yaml`). The native script's contract is
# *staged-file scope* only, mirroring what a typical contributor
# wants on every commit.
#
# Tools run (each conditional on availability — missing tools are
# skipped with a one-line notice, never block the commit):
#
#   - ruff check --fix         (Python: every .py / .pyi; the declared exceptions are skipped)
#   - clang-format -i          (C/C++/CUDA/HIP/Metal: core/, cmd/, ffmpeg-patches src)
#   - shfmt -w -i 2 -ci        (shell: scripts/, dev/, top-level *.sh)
#
# Re-stages files that the formatters touched so the commit picks up
# the auto-fixes. Reports per-file fix count at the end.
#
# Bypass with `git commit --no-verify` (standard escape hatch).
#
# See docs/development/pre-commit-hooks.md and ADR-0924.

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || repo_root=""
if [ -z "${repo_root}" ]; then
  # Not in a git repo — nothing to do.
  exit 0
fi
cd "${repo_root}"

# ---- Collect staged files by type --------------------------------------------

# diff-filter=ACM: Added, Copied, Modified — skip Deleted/Renamed targets.
mapfile -t staged < <(git diff --cached --name-only --diff-filter=ACM)

if [ "${#staged[@]}" -eq 0 ]; then
  # Nothing staged (e.g. `git commit --allow-empty`). Exit clean.
  exit 0
fi

py_files=()
c_files=()
sh_files=()

for f in "${staged[@]}"; do
  # Skip files that disappeared between staging and now (rare race —
  # e.g. concurrent edit). Formatting a missing file would noisily fail.
  [ -f "$f" ] || continue

  # Python — every file, as the .pre-commit-config.yaml ruff hook reads it.
  # The declared exceptions (.config/lint-exceptions.d/ruff.toml) are
  # skipped by `ruff --force-exclude` below through pyproject.toml's
  # extend-exclude. Regex rather than case-globs: case `*` matches `/` and
  # produces SC2221/SC2222 shellcheck warnings.
  if [[ "$f" =~ \.pyi?$ ]]; then
    py_files+=("$f")
    continue
  fi

  # C / C++ / CUDA / HIP / Metal — clang-format honours .clang-format.
  # subprojects/ and core/test/data/ are excluded to mirror the
  # framework hook's exclude pattern (upstream-vendored sources);
  # scripts/ci/exact_twins.d/ holds parity-gate data fragments that only
  # borrow the .hip extension.
  if [[ "$f" =~ \.(c|h|cpp|hpp|cc|cu|cuh|hip|metal)$ ]]; then
    if [[ "$f" =~ ^(subprojects/|core/test/data/|scripts/ci/exact_twins\.d/) ]]; then
      continue
    fi
    c_files+=("$f")
    continue
  fi

  # Shell — shfmt honours -i/-ci args; .shfmt would override. The declared
  # exceptions (.config/lint-exceptions.d/shfmt.toml, reason per entry) are
  # skipped, as the framework hook's exclude skips them.
  if [[ "$f" =~ \.(sh|bash)$ ]]; then
    if [[ "$f" =~ ^\.config/lefthook/python\.sh$ ]]; then
      continue
    fi
    sh_files+=("$f")
    continue
  fi
done

# ---- Track which files actually changed -------------------------------------

changed_count=0
declare -A changed_by_tool

# Run one formatter over the files. Returns the tool's own status, or 2 for an
# unknown tool name.
_apply_formatter() {
  local tool="$1"
  local binary="$2"
  shift 2

  case "$tool" in
    ruff)
      # --fix to apply autofixes, --quiet to suppress per-file noise,
      # --force-exclude so the declared exceptions in pyproject.toml hold for
      # explicitly named files. Non-zero exit on unfixable findings —
      # propagate to block commit.
      "$binary" check --fix --quiet --force-exclude -- "$@" || return $?
      ;;
    clang-format)
      "$binary" -i -- "$@"
      ;;
    shfmt)
      "$binary" -w -i 2 -ci -- "$@"
      ;;
    *)
      printf '[pre-commit] internal error: unknown tool %s\n' "$tool" >&2
      return 2
      ;;
  esac
}

# Re-stage every file whose hash differs between the two snapshots.
# $1 = tool name, $2 = hashes before, $3 = hashes after, $@>=4 = files (same
# order as the hash lists).
_restage_changed() {
  local tool="$1"
  local before_hashes="$2"
  local after_hashes="$3"
  shift 3
  local -a files=("$@")
  local tool_changed=0
  local i=0
  # Parse line-by-line; both lists are in the same order as "${files[@]}".
  local -a before_arr after_arr
  mapfile -t before_arr <<<"$before_hashes"
  mapfile -t after_arr <<<"$after_hashes"

  for ((i = 0; i < ${#files[@]}; i++)); do
    if [ "${before_arr[$i]:-}" != "${after_arr[$i]:-}" ]; then
      git add -- "${files[$i]}"
      tool_changed=$((tool_changed + 1))
      changed_count=$((changed_count + 1))
    fi
  done
  changed_by_tool["$tool"]="$tool_changed"
}

run_formatter() {
  # $1 = tool name (display)
  # $2 = binary to look up on PATH
  # $@>=3 = files to pass (one or more)
  local tool="$1"
  local binary="$2"
  shift 2
  local files=("$@")

  if [ "${#files[@]}" -eq 0 ]; then
    return 0
  fi

  if ! command -v "$binary" >/dev/null 2>&1; then
    printf '[pre-commit] %s: not installed — skipping %d file(s)\n' \
      "$tool" "${#files[@]}" >&2
    return 0
  fi

  # Snapshot pre-tool hashes for change detection.
  local before_hashes after_hashes
  before_hashes="$(git hash-object -- "${files[@]}" 2>/dev/null)" || before_hashes=""

  _apply_formatter "$tool" "$binary" "${files[@]}" || return $?

  # Compute per-file delta. Re-stage touched files.
  after_hashes="$(git hash-object -- "${files[@]}" 2>/dev/null)" || after_hashes=""
  _restage_changed "$tool" "$before_hashes" "$after_hashes" "${files[@]}"
}

# ---- Run formatters ---------------------------------------------------------

run_formatter ruff ruff "${py_files[@]}"
run_formatter clang-format clang-format "${c_files[@]}"
run_formatter shfmt shfmt "${sh_files[@]}"

# ---- Summary ----------------------------------------------------------------

if [ "$changed_count" -gt 0 ]; then
  printf '[pre-commit] auto-fixed and re-staged %d file(s):' "$changed_count" >&2
  for tool in "${!changed_by_tool[@]}"; do
    count="${changed_by_tool[$tool]}"
    if [ "$count" -gt 0 ]; then
      printf ' %s=%d' "$tool" "$count" >&2
    fi
  done
  printf '\n' >&2
fi

exit 0
