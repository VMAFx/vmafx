#!/usr/bin/env bash
# PostToolUse hook (matcher: Edit|Write): warn when an edit touches a
# user-discoverable surface (per CLAUDE.md §12 rule 10 / ADR-0100) but
# no matching `docs/<topic>/` file has been touched in this session.
#
# Informational only — does not block the edit. Mirrors
# auto-snapshot-warn.sh's pattern. The CI counterpart in
# `.github/workflows/rule-enforcement.yml` is the blocking gate
# (ADR-0167).
#
# Surface → docs path mapping is intentionally narrow; pure internal
# refactors with no user-visible delta legitimately need no docs and
# the warning is just a reminder, not an accusation.
set -euo pipefail

file="${CLAUDE_TOOL_INPUT_file_path:-}"
if [[ -z "$file" ]] && command -v jq >/dev/null 2>&1; then
  input=$(cat 2>/dev/null || true)
  if [[ -n "$input" ]]; then
    file=$(printf '%s' "$input" | jq -r '.tool_input.file_path // empty' 2>/dev/null || true)
  fi
fi
[[ -z "$file" ]] && exit 0

repo_root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
rel="${file#"${repo_root}"/}"

# Map the touched surface to the expected docs/ path. If the file is
# not a user-discoverable surface, exit silently.
expected_doc=""
case "$rel" in
  core/include/libvmaf/libvmaf_cuda.h | core/include/libvmaf/libvmaf_sycl.h)
    expected_doc="docs/api/gpu.md"
    ;;
  core/include/libvmaf/libvmaf.h | core/include/libvmaf/picture.h | \
    core/include/libvmaf/model.h)
    expected_doc="docs/api/index.md"
    ;;
  core/include/libvmaf/libvmaf_dnn.h)
    expected_doc="docs/api/dnn.md"
    ;;
  core/src/feature/feature_*.c | core/src/feature/integer_*.c | \
    core/src/feature/x86/*.c | core/src/feature/arm64/*.c | \
    core/src/feature/cuda/*.c | core/src/feature/cuda/*.cu | \
    core/src/feature/sycl/*.cpp)
    expected_doc="docs/metrics/features.md"
    ;;
  core/tools/cli_parse.c | core/tools/cli_parse.cpp | core/tools/vmaf.cpp | core/tools/vmaf_bench.c)
    expected_doc="docs/usage/cli.md"
    ;;
  meson_options.txt | core/meson_options.txt)
    expected_doc="docs/development/build-flags.md"
    ;;
  mcp-server/vmaf-mcp/src/*.py | mcp-server/vmaf-mcp/pyproject.toml)
    expected_doc="docs/mcp/"
    ;;
  ai/src/vmaf_train/cli/*.py)
    expected_doc="docs/ai/"
    ;;
  ffmpeg-patches/*.patch)
    expected_doc="docs/usage/"
    ;;
  *) exit 0 ;;
esac

# Has anything under that docs path been edited in the working tree
# during this session? Compare modification times against the surface
# file: if expected_doc is older than the surface, warn.
abs_doc="${repo_root}/${expected_doc%/}"

# expected_doc may be a directory (e.g. docs/mcp/) or a single file.
if [[ -d "$abs_doc" ]]; then
  # Any file under the directory newer than the surface counts.
  if find "$abs_doc" -type f -newer "$file" -print -quit 2>/dev/null | grep -q .; then
    exit 0
  fi
elif [[ -f "$abs_doc" ]]; then
  if [[ "$abs_doc" -nt "$file" ]]; then
    exit 0
  fi
fi

# Also: if the working tree shows a staged or unstaged change to the
# expected docs file, count that as covered.
if git -C "$repo_root" status --porcelain -- "$expected_doc" 2>/dev/null | grep -q .; then
  exit 0
fi

cat >&2 <<EOF
NOTICE: docs drift candidate — edited user-discoverable surface: $rel

Per CLAUDE.md §12 rule 10 / ADR-0100, every PR that changes a
user-discoverable surface ships matching documentation in the SAME PR.
Expected docs path for this surface: $expected_doc

If this edit is a pure internal refactor / bug fix with no user-visible
delta, no docs are required (see ADR-0100 § Context for the exemption).
Otherwise, update the docs path above before opening the PR — the CI
'doc-substance-check' job will reject the PR otherwise (ADR-0167).
EOF

exit 0
