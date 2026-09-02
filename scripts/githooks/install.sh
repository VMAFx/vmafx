#!/usr/bin/env bash
# scripts/githooks/install.sh — install git hooks (framework or native).
#
# Default behaviour matches the historical `make install-hooks` target:
# install the pre-commit-framework hook from .pre-commit-config.yaml and
# the commit-msg hook for Conventional Commits enforcement, plus the
# fork's pre-push PR-body deliverables validator (ADR-0108).
#
# Opt-in to the native bash pre-commit by exporting
# VMAFX_NATIVE_HOOKS=1 before invocation. The native hook is ~10x
# faster on small commits (no per-hook venv wrap, ADR-0924). CI is
# unaffected — it continues to invoke `pre-commit run --all-files`
# against the framework config.
#
# Both paths preserve any existing user-managed pre-push hook by
# moving it aside as `pre-push.local-backup` rather than silently
# overwriting it.
#
# Usage:
#   ./scripts/githooks/install.sh                  # framework hook
#   VMAFX_NATIVE_HOOKS=1 ./scripts/githooks/install.sh  # native hook

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)"
hooks_dir="$(git rev-parse --git-path hooks)"
mkdir -p "$hooks_dir"

# ---- pre-push (PR-body deliverables validator) — installed in both modes ----

prepush_src="${repo_root}/scripts/git-hooks/pre-push"
prepush_dst="${hooks_dir}/pre-push"

if [ -x "$prepush_src" ]; then
  if [ -e "$prepush_dst" ] && [ ! -L "$prepush_dst" ]; then
    echo "install.sh: preserving existing $prepush_dst as $prepush_dst.local-backup"
    mv "$prepush_dst" "$prepush_dst.local-backup"
  fi
  ln -sfn "$prepush_src" "$prepush_dst"
  echo "install.sh: pre-push -> $prepush_src"
else
  echo "install.sh: WARNING — $prepush_src missing or not executable; pre-push not installed" >&2
fi

# ---- pre-commit: native vs framework ---------------------------------------

if [ "${VMAFX_NATIVE_HOOKS:-0}" = "1" ]; then
  # Native path — symlink directly to the bash script. No venv, no
  # framework startup cost. Per-file formatters chosen at run time.
  native_src="${repo_root}/scripts/githooks/pre-commit.sh"
  native_dst="${hooks_dir}/pre-commit"

  if [ ! -x "$native_src" ]; then
    echo "install.sh: ERROR — $native_src missing or not executable" >&2
    exit 1
  fi

  if [ -e "$native_dst" ] && [ ! -L "$native_dst" ]; then
    echo "install.sh: preserving existing $native_dst as $native_dst.local-backup"
    mv "$native_dst" "$native_dst.local-backup"
  fi
  ln -sfn "$native_src" "$native_dst"
  echo "install.sh: NATIVE pre-commit -> $native_src"
  echo "install.sh: (CI continues to use the pre-commit framework; this only affects local commits.)"
else
  # Framework path — invoke pre-commit to populate .git/hooks/pre-commit
  # and .git/hooks/commit-msg from .pre-commit-config.yaml.
  if ! command -v pre-commit >/dev/null 2>&1; then
    echo "install.sh: pre-commit not on PATH — attempting 'pip install pre-commit'"
    pip install pre-commit
  fi
  pre-commit install --install-hooks
  pre-commit install --hook-type commit-msg
  echo "install.sh: FRAMEWORK pre-commit installed (set VMAFX_NATIVE_HOOKS=1 to opt into the native path)"
fi
