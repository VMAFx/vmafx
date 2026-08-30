#!/usr/bin/env bash
#
# scaffold.sh — materialize the add-mcp-tool templates for a named tool.
#
# Usage: bash .claude/skills/add-mcp-tool/scaffold.sh <name>
#   where <name> is snake_case and prefixed by domain (vmaf_*, tune_*, model_*, health_*).
#
# Generates parallel Go and Python handler stubs PLUS the per-tool doc page so
# the per-surface doc rule (ADR-0100) is satisfied at scaffold time.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <name>" >&2
  exit 2
fi

name="$1"
if [[ ! "$name" =~ ^[a-z]+_[a-z0-9_]+$ ]]; then
  echo "error: <name> must be snake_case with a domain prefix (vmaf_*, tune_*, model_*, health_*)" >&2
  exit 2
fi

# snake_case → PascalCase (vmaf_score → VmafScore).
name_pascal=$(printf '%s\n' "$name" | awk -F_ '{for (i=1; i<=NF; i++) printf "%s%s", toupper(substr($i,1,1)), substr($i,2); print ""}')
name_upper="${name^^}"

repo_root=$(git rev-parse --show-toplevel)
tpl="$repo_root/.claude/skills/add-mcp-tool/templates"

go_dir="$repo_root/cmd/vmafx-mcp"
py_dir="$repo_root/mcp-server/vmaf-mcp/src/vmaf_mcp/tools"
py_test_dir="$repo_root/mcp-server/vmaf-mcp/tests"
doc_dir="$repo_root/docs/mcp/tools"
changelog_dir="$repo_root/changelog.d/added"

# Refuse to clobber existing files — scaffolds are write-once.
for f in \
  "$go_dir/impl_${name}.go" \
  "$go_dir/impl_${name}_test.go" \
  "$py_dir/${name}.py" \
  "$py_test_dir/test_${name}.py" \
  "$doc_dir/${name}.md" \
  "$changelog_dir/mcp-tool-${name}.md"; do
  if [[ -e "$f" ]]; then
    echo "error: $f already exists; refusing to overwrite" >&2
    exit 3
  fi
done

mkdir -p "$py_dir" "$py_test_dir" "$doc_dir" "$changelog_dir"

subst() {
  sed -e "s/@NAME@/$name/g" \
    -e "s/@NAME_UPPER@/$name_upper/g" \
    -e "s/@NAME_PASCAL@/$name_pascal/g" \
    "$@"
}

subst "$tpl/impl_go.template" >"$go_dir/impl_${name}.go"
subst "$tpl/impl_go_test.template" >"$go_dir/impl_${name}_test.go"
subst "$tpl/impl_py.template" >"$py_dir/${name}.py"
subst "$tpl/test_py.template" >"$py_test_dir/test_${name}.py"
subst "$tpl/doc.template" >"$doc_dir/${name}.md"

cat >"$changelog_dir/mcp-tool-${name}.md" <<EOF
- Added new MCP tool \`${name}\` (Go + Python handlers in parity); see [docs/mcp/tools/${name}.md](docs/mcp/tools/${name}.md).
EOF

echo "scaffolded MCP tool '$name':"
echo "  go    : $go_dir/impl_${name}.go (+ test)"
echo "  py    : $py_dir/${name}.py (+ test)"
echo "  doc   : $doc_dir/${name}.md"
echo "  log   : $changelog_dir/mcp-tool-${name}.md"
echo
echo "next steps:"
echo "  1. register the tool in cmd/vmafx-mcp/tools.go (addRawTool + Handler dispatch)"
echo "  2. register the tool in mcp-server/vmaf-mcp/src/vmaf_mcp/server.py (_list_tools + _call_tool)"
echo "  3. add the new row to docs/mcp/tools.md (sorted by domain, then name)"
echo "  4. bump the tool count in docs/mcp/index.md overview paragraph"
echo "  5. note the parity contract entry in cmd/vmafx-mcp/AGENTS.md + mcp-server/AGENTS.md"
echo "  6. fill the TODO blocks in the Go + Python handlers AND the doc page"
echo "  7. run: go test ./cmd/vmafx-mcp/... && pytest mcp-server/vmaf-mcp/tests/test_${name}.py"
