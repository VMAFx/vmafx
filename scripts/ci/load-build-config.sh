#!/usr/bin/env bash
# Export every knob in build-config.env into the GitHub Actions environment.
#
# GitHub Actions cannot `source` a file at workflow-parse time, so a workflow
# that needs a version has historically hardcoded it -- and then drifted. This
# is the bridge: call it in a `run:` step and every later step in the same job
# can use ${ONEAPI_VERSION}, ${ROCM_APT_VERSION}, ${PYTHON_CI_VERSION} and the
# rest instead of a literal.
#
#   - name: Load build config
#     run: scripts/ci/load-build-config.sh >> "$GITHUB_ENV"
#
# It writes `KEY=value` lines to stdout, so redirect it into $GITHUB_ENV. Run
# outside Actions it just prints the resolved config, which is what makes it
# testable.
#
# Values are printed unquoted because $GITHUB_ENV takes them literally; none of
# the knobs contain a newline, and the check below enforces that.
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
config="$repo_root/build-config.env"

if [ ! -f "$config" ]; then
  echo "load-build-config: $config not found" >&2
  exit 2
fi

# shellcheck disable=SC1090
. "$config"

while IFS= read -r key; do
  value="${!key-}"
  case "$value" in
    *$'\n'*)
      echo "load-build-config: $key contains a newline; \$GITHUB_ENV cannot carry it" >&2
      exit 1
      ;;
  esac
  printf '%s=%s\n' "$key" "$value"
done < <(grep -oE '^[A-Z][A-Z0-9_]*=' "$config" | tr -d '=' | sort -u)
