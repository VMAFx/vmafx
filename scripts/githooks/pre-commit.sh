#!/usr/bin/env bash
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
#   - ruff check --fix         (Python: ai/, scripts/, tools/, python/)
#   - clang-format -i          (C/C++/CUDA: core/, cmd/, ffmpeg-patches src)
#   - shfmt -w -i 2 -ci        (shell: scripts/, dev/, top-level *.sh)
#
# Re-stages files that the formatters touched so the commit picks up
# the auto-fixes. Reports per-file fix count at the end.
#
# Bypass with `git commit --no-verify` (standard escape hatch).
#
# See docs/development/pre-commit-hooks.md and ADR-0924.

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
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

  # Python — fork-added trees only. Mirrors .pre-commit-config.yaml
  # ruff hook scope (^(python|ai|scripts|tools)/.*\.py$). Use regex
  # rather than case-globs because case `*` matches `/` and produces
  # SC2221/SC2222 shellcheck warnings when listing multi-segment
  # patterns side-by-side.
  if [[ "$f" =~ ^(python|ai|scripts|tools)/.*\.py$ ]]; then
    py_files+=("$f")
    continue
  fi

  # C / C++ / CUDA — clang-format honours .clang-format.
  # subprojects/ and core/test/data/ are excluded to mirror the
  # framework hook's exclude pattern (upstream-vendored sources).
  if [[ "$f" =~ \.(c|h|cpp|hpp|cc|cu|cuh)$ ]]; then
    if [[ "$f" =~ ^(subprojects/|core/test/data/) ]]; then
      continue
    fi
    c_files+=("$f")
    continue
  fi

  # Shell — shfmt honours -i/-ci args; .shfmt would override.
  if [[ "$f" =~ \.(sh|bash)$ ]]; then
    sh_files+=("$f")
    continue
  fi
done

# ---- Track which files actually changed -------------------------------------

changed_count=0
declare -A changed_by_tool

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
  local before_hashes
  before_hashes="$(git hash-object -- "${files[@]}" 2>/dev/null || true)"

  case "$tool" in
    ruff)
      # --fix to apply autofixes, --quiet to suppress per-file noise.
      # Non-zero exit on unfixable findings — propagate to block commit.
      "$binary" check --fix --quiet -- "${files[@]}" || return $?
      ;;
    clang-format)
      "$binary" -i -- "${files[@]}"
      ;;
    shfmt)
      "$binary" -w -i 2 -ci -- "${files[@]}"
      ;;
    *)
      printf '[pre-commit] internal error: unknown tool %s\n' "$tool" >&2
      return 2
      ;;
  esac

  # Compute per-file delta. Re-stage touched files.
  local after_hashes
  after_hashes="$(git hash-object -- "${files[@]}" 2>/dev/null || true)"

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
