#!/usr/bin/env bash
# container-build.sh — build the dev container from a verified source revision.
#
# `docker compose build dev-mcp` works, but it will happily build from a
# checkout that is behind master and produce an image that looks fresh by
# timestamp while missing the very commits the rebuild was for (ADR-1195).
# This wrapper closes that gap: it checks the build context first, then passes
# the revision it verified into the image so the result is self-describing.
#
# Usage:
#   dev/scripts/container-build.sh [--allow-behind] [--ref REF] [-- ARGS...]
#
#   --allow-behind   Build anyway when the checkout is behind REF. For local
#                    experiments on a branch. Never for an image whose output
#                    will be published or cited.
#   --ref REF        Compare against REF instead of origin/master.
#   -- ARGS...       Passed through to `docker compose build`.
set -euo pipefail

allow_behind=0
ref="origin/master"
passthrough=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-behind)
      allow_behind=1
      shift
      ;;
    --ref)
      ref="${2:?--ref needs a git ref}"
      shift 2
      ;;
    --)
      shift
      passthrough=("$@")
      break
      ;;
    -h | --help)
      sed -n '2,18p' "$0"
      exit 0
      ;;
    *)
      echo "container-build: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

if ! bash scripts/dev/check-container-source.sh --pre-build --ref "$ref"; then
  if [[ "$allow_behind" -eq 0 ]]; then
    echo "" >&2
    echo "container-build: refusing to build from a stale context." >&2
    echo "  Re-run with --allow-behind if you really want this image." >&2
    exit 1
  fi
  echo "container-build: WARNING: building from a stale context (--allow-behind)." >&2
fi

VMAFX_SOURCE_REV="$(git rev-parse HEAD)"
VMAFX_SOURCE_REF="$ref"
export VMAFX_SOURCE_REV VMAFX_SOURCE_REF

echo "container-build: building from ${VMAFX_SOURCE_REV:0:9} (context checked against $ref)"
docker compose --project-directory "$repo_root" -f dev/docker-compose.yml \
  build "${passthrough[@]:-dev-mcp}"

echo ""
echo "container-build: verifying the built image records that revision"
bash scripts/dev/check-container-source.sh --image vmaf-dev-mcp:local --ref "$ref" --no-fetch
