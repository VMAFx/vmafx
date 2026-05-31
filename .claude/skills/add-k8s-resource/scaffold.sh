#!/usr/bin/env bash
#
# scaffold.sh — materialize the add-k8s-resource templates for a named CRD.
#
# Usage: bash .claude/skills/add-k8s-resource/scaffold.sh <KindName>
#   where <KindName> is PascalCase WITHOUT the Vmafx prefix.
#
# Example: bash scaffold.sh BenchmarkRun
#   → CRD: VmafxBenchmarkRun, plural: vmafxbenchmarkruns, short: vmbenc
#
# Override the auto-pluralisation with K8S_PLURAL_OVERRIDE=<plural>.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <KindName>" >&2
  exit 2
fi

raw_kind="$1"
if [[ ! "$raw_kind" =~ ^[A-Z][A-Za-z0-9]+$ ]]; then
  echo "error: <KindName> must be PascalCase, no leading 'Vmafx' (the scaffold adds it)" >&2
  exit 2
fi
if [[ "$raw_kind" == Vmafx* ]]; then
  echo "error: drop the 'Vmafx' prefix from <KindName>; the scaffold adds it" >&2
  exit 2
fi

kind="Vmafx${raw_kind}"
# PascalCase → lowercase (VmafxBenchmarkRun → vmafxbenchmarkrun) for paths.
kind_lower=$(printf '%s\n' "$kind" | tr '[:upper:]' '[:lower:]')
plural="${K8S_PLURAL_OVERRIDE:-${kind_lower}s}"
# Short name: vm + first 4 chars of raw_kind, lowercase. Bounded to be friendly to kubectl.
short_suffix=$(printf '%s' "$raw_kind" | tr '[:upper:]' '[:lower:]' | cut -c1-4)
short="vm${short_suffix}"

repo_root=$(git rev-parse --show-toplevel)
tpl="$repo_root/.claude/skills/add-k8s-resource/templates"

api_dir="$repo_root/api/vmafx/v1"
ctl_dir="$repo_root/cmd/vmafx-operator/internal/controller"
crd_dir="$repo_root/deploy/helm/vmafx/crds"
rbac_dir="$repo_root/deploy/helm/vmafx/templates"
doc_dir="$repo_root/docs/k8s/crds"
changelog_dir="$repo_root/changelog.d/added"

for f in \
  "$api_dir/${kind_lower}_types.go" \
  "$ctl_dir/${kind_lower}_controller.go" \
  "$ctl_dir/${kind_lower}_controller_test.go" \
  "$crd_dir/vmafx.dev_${plural}.yaml" \
  "$rbac_dir/operator-rbac-${kind_lower}.yaml" \
  "$doc_dir/${kind_lower}.md" \
  "$changelog_dir/k8s-crd-${kind_lower}.md"; do
  if [[ -e "$f" ]]; then
    echo "error: $f already exists; refusing to overwrite" >&2
    exit 3
  fi
done

mkdir -p "$api_dir" "$ctl_dir" "$crd_dir" "$rbac_dir" "$doc_dir" "$changelog_dir"

subst() {
  sed -e "s/@KIND@/$kind/g" \
    -e "s/@KIND_LOWER@/$kind_lower/g" \
    -e "s/@PLURAL@/$plural/g" \
    -e "s/@SHORT@/$short/g" \
    "$@"
}

subst "$tpl/types.go.template" >"$api_dir/${kind_lower}_types.go"
subst "$tpl/controller.go.template" >"$ctl_dir/${kind_lower}_controller.go"
subst "$tpl/controller_test.go.template" >"$ctl_dir/${kind_lower}_controller_test.go"
subst "$tpl/crd.yaml.template" >"$crd_dir/vmafx.dev_${plural}.yaml"
subst "$tpl/rbac.yaml.template" >"$rbac_dir/operator-rbac-${kind_lower}.yaml"
subst "$tpl/doc.md.template" >"$doc_dir/${kind_lower}.md"

cat >"$changelog_dir/k8s-crd-${kind_lower}.md" <<EOF
- Added new Kubernetes CRD \`${kind}\` (group \`vmafx.dev\`, plural \`${plural}\`, short name \`${short}\`); see [docs/k8s/crds/${kind_lower}.md](docs/k8s/crds/${kind_lower}.md).
EOF

echo "scaffolded CRD '$kind':"
echo "  types       : $api_dir/${kind_lower}_types.go"
echo "  controller  : $ctl_dir/${kind_lower}_controller.go (+ test)"
echo "  crd manifest: $crd_dir/vmafx.dev_${plural}.yaml"
echo "  rbac        : $rbac_dir/operator-rbac-${kind_lower}.yaml"
echo "  doc         : $doc_dir/${kind_lower}.md"
echo "  changelog   : $changelog_dir/k8s-crd-${kind_lower}.md"
echo
echo "next steps:"
echo "  1. wire SetupWithManager into cmd/vmafx-operator/main.go"
echo "  2. extend the --enable-controllers flag whitelist in main.go"
echo "  3. add operator.controllers.${kind_lower}.enabled=false to deploy/helm/vmafx/values.yaml"
echo "  4. append a row to docs/development/operator.md controller table"
echo "  5. note ${kind} in cmd/vmafx-operator/AGENTS.md 'controllers shipped' invariant"
echo "  6. fill the TODO blocks in types.go / controller.go / crd.yaml / doc.md"
echo "  7. run: make manifests && go build ./cmd/vmafx-operator/... && go test ./cmd/vmafx-operator/..."
echo "  8. verify helm renders: helm template deploy/helm/vmafx --set operator.enabled=true --set operator.controllers.${kind_lower}.enabled=true | grep -A2 '${kind}'"
