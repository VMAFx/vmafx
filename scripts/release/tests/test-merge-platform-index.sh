#!/usr/bin/env bash
# Regression tests for .github/actions/merge-platform-index, the step the
# four publish jobs of docker-publish-operator-node.yml merge their platform
# images with.
#
# The action's run block is executed against a stub `docker` on PATH that
# logs its calls; each defect is planted on its own and must be refused.
# No Docker, no network.
#
# Usage: bash scripts/release/tests/test-merge-platform-index.sh
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ACTION="$SCRIPT_DIR/../../../.github/actions/merge-platform-index/action.yml"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0
ok() {
  pass=$((pass + 1))
  printf 'ok   %s\n' "$1"
}
bad() {
  fail=$((fail + 1))
  printf 'FAIL %s\n' "$1" >&2
}

# The run block of the action's one step, de-indented.
python3 - "$ACTION" "$scratch/run.sh" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
block = re.search(r"(?ms)^      run: \|\n((?:        .*\n|\n)+)", text)
if block is None:
    raise SystemExit("merge-platform-index: no run block")
Path(sys.argv[2]).write_text(re.sub(r"(?m)^        ", "", block.group(1)), encoding="utf-8")
PY

mkdir -p "$scratch/bin"
cat >"$scratch/bin/docker" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$SHIM_LOG"
if [[ "$*" == *"--format"* ]]; then
  printf '{"digest":"sha256:feed"}\n'
fi
exit "${SHIM_CREATE_RC:-0}"
STUB
chmod +x "$scratch/bin/docker"

# merge EXPECTED_RC DESCRIPTION DIGEST_COUNT [ENV=VALUE...]
merge() {
  local want="$1" description="$2" count="$3" rc=0 n
  shift 3
  rm -rf "$scratch/digests" "$scratch/log" "$scratch/output"
  mkdir -p "$scratch/digests"
  for ((n = 0; n < count; n++)); do : >"$scratch/digests/d$n"; done
  (cd "$scratch/digests" && env PATH="$scratch/bin:$PATH" SHIM_LOG="$scratch/log" \
    GITHUB_OUTPUT="$scratch/output" IMAGE=ghcr.io/vmafx/vmafx-node TAG=v0.1.0 PLATFORMS=2 \
    META_JSON='{"tags":["ghcr.io/vmafx/vmafx-node:v0.1.0","ghcr.io/vmafx/vmafx-node:latest"]}' \
    "$@" bash -euo pipefail "$scratch/run.sh") >/dev/null 2>"$scratch/err" || rc=$?
  if [[ "$rc" -eq "$want" ]]; then ok "$description"; else bad "$description (exit $rc, wanted $want: $(cat "$scratch/err"))"; fi
}

merge 0 "merges two platform digests" 2
if grep -qx 'buildx imagetools create --tag ghcr.io/vmafx/vmafx-node:v0.1.0 --tag ghcr.io/vmafx/vmafx-node:latest ghcr.io/vmafx/vmafx-node@sha256:d0 ghcr.io/vmafx/vmafx-node@sha256:d1' "$scratch/log"; then
  ok "applies every metadata tag to both platform images"
else
  bad "unexpected imagetools create call: $(head -1 "$scratch/log")"
fi
if grep -qx 'digest=sha256:feed' "$scratch/output"; then ok "returns the index digest"; else bad "no digest output"; fi
merge 1 "refuses one platform digest" 1
merge 1 "refuses three platform digests" 3
merge 1 "refuses an empty digest folder" 0
merge 0 "honours a different platform count" 3 PLATFORMS=3
merge 3 "fails when imagetools create fails" 2 SHIM_CREATE_RC=3

printf '%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
