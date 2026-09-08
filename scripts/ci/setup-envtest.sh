#!/usr/bin/env bash
# Copyright 2026 Lusoris
# Copyright 2026 Claude (Anthropic)
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Install/use the canonical envtest tool without accepting a stale PATH binary.
set -euo pipefail

mode="${1:-}"
if [[ $# != 1 || ! "$mode" =~ ^(install|path|env)$ ]]; then
  echo "usage: $0 {install|path|env}" >&2
  exit 2
fi
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
requested_k8s="${ENVTEST_K8S_VERSION:-}"
# ADR-1231: runtime config is exercised by the shared consumer regression.
# shellcheck source=/dev/null
. "$repo_root/build-config.env"
requested_k8s="${requested_k8s:-$ENVTEST_K8S_VERSION}"
if [[ ! "$SETUP_ENVTEST_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "setup-envtest: expected an exact release version in build-config.env" >&2
  exit 2
fi
command -v go >/dev/null || {
  echo "setup-envtest: Go is required" >&2
  exit 1
}

# Go install's documented destination. This helper serves the Bash/Make and
# Ubuntu CI paths; use the first entry when GOPATH contains several roots.
tool_dir="$(go env GOBIN)"
if [[ -z "$tool_dir" ]]; then
  go_path="$(go env GOPATH)"
  if [[ -z "$go_path" ]]; then
    echo "setup-envtest: Go returned an empty GOPATH" >&2
    exit 1
  fi
  tool_dir="${go_path%%:*}/bin"
fi
if [[ "$tool_dir" != /* ]]; then
  echo "setup-envtest: Go installation directory must be an absolute POSIX path" >&2
  exit 1
fi
tool="$tool_dir/setup-envtest"
module="sigs.k8s.io/controller-runtime/tools/setup-envtest"
if [[ "$mode" == install ]]; then
  GOBIN="$tool_dir" go install "$module@$SETUP_ENVTEST_VERSION"
fi
if [[ ! -x "$tool" ]]; then
  echo "setup-envtest: run make setup-envtest to install $SETUP_ENVTEST_VERSION" >&2
  exit 1
fi
metadata="$(go version -m "$tool")"
installed_version="$(awk -v module="$module" '$1 == "mod" && $2 == module { print $3 }' <<<"$metadata")"
if [[ "$installed_version" != "$SETUP_ENVTEST_VERSION" ]]; then
  echo "setup-envtest: installed tool differs from $SETUP_ENVTEST_VERSION; run make setup-envtest" >&2
  exit 1
fi
asset_args=(use "$requested_k8s" -p path --use-env=false)
if [[ "$mode" != install ]]; then
  asset_args+=(--installed-only)
fi
assets="$("$tool" "${asset_args[@]}")"
if [[ -z "$assets" ]]; then
  echo "setup-envtest: no asset path returned" >&2
  exit 1
fi
if [[ "$mode" == env ]]; then
  printf 'export KUBEBUILDER_ASSETS=%q\n' "$assets"
else
  printf '%s\n' "$assets"
fi
