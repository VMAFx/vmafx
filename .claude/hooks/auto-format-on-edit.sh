#!/usr/bin/env bash
# PostToolUse hook (matcher: Edit|Write): auto-format files after the agent edits them.
# Uses repo-local tool versions when available; silently skips if a formatter is not installed.
set -euo pipefail

# Claude Code passes hook input as JSON on stdin. Parse tool_input.file_path
# from it. Older docs referenced CLAUDE_TOOL_INPUT_file_path env var; keep
# it as a fallback for compatibility with any external caller.
file="${CLAUDE_TOOL_INPUT_file_path:-}"
if [[ -z "$file" ]] && command -v jq >/dev/null 2>&1; then
  # Read stdin (non-blocking — the hook runtime always provides it).
  input=$(cat 2>/dev/null || true)
  if [[ -n "$input" ]]; then
    file=$(printf '%s' "$input" | jq -r '.tool_input.file_path // empty' 2>/dev/null || true)
  fi
fi

[[ -z "$file" || ! -f "$file" ]] && exit 0

# Never reformat files that are explicitly upstream-touched or generated
case "$file" in
  */subprojects/* | */build/* | */testdata/*.yuv | *.json | *.onnx | *.pkl)
    exit 0
    ;;
esac

case "$file" in
  *.c | *.h | *.cpp | *.hpp | *.cu | *.cuh)
    if command -v clang-format >/dev/null 2>&1; then
      clang-format -i --style=file "$file" || true
    fi
    ;;
  *.py)
    if command -v black >/dev/null 2>&1; then
      black -q "$file" || true
    fi
    if command -v isort >/dev/null 2>&1; then
      isort -q "$file" || true
    fi
    ;;
  *.sh)
    if command -v shfmt >/dev/null 2>&1; then
      shfmt -w -i 2 -ci "$file" || true
    fi
    ;;
esac

exit 0
