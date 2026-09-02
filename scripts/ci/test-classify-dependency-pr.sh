#!/usr/bin/env bash
# scripts/ci/test-classify-dependency-pr.sh — test suite for classify-dependency-pr.sh (ADR-1152).
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# Exercises the dependency PR classifier against canonical test cases:
#   - Dependency-only manifests / lockfiles -> exempt (exit 0)
#   - Mixed set (manifest + source code) -> not exempt (exit 1)
#   - Non-bot author without bot branch -> not exempt (exit 1)
#   - Bot author with non-manifest doc / source -> not exempt (exit 1)
#   - Dependabot bot author -> exempt (exit 0)
#   - Head branch matching renovate/* -> exempt (exit 0)
#
# Run from anywhere:
#   bash scripts/ci/test-classify-dependency-pr.sh

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
classifier="${repo_root}/scripts/ci/classify-dependency-pr.sh"

if [ ! -x "${classifier}" ]; then
  echo "test-classify-dependency-pr: classifier missing or not executable at ${classifier}" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

pass_count=0
fail_count=0

expect_exit() {
  local name="$1" expected="$2" author="$3" branch="$4" diff_file="$5"
  local out exit_code=0
  out="$("${classifier}" --author "${author}" --branch "${branch}" --diff "${diff_file}" 2>&1)" || exit_code=$?
  if [ "${exit_code}" -eq "${expected}" ]; then
    echo "PASS: ${name} (exit=${exit_code})"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} (got exit=${exit_code}, expected ${expected})"
    echo "----- captured output -----"
    printf '%s\n' "${out}"
    echo "---------------------------"
    fail_count=$((fail_count + 1))
  fi
}

# Case 1: Dependency-only path set -> exempt
cat >"${work}/case1_dep_only.diff" <<'DIFF'
package.json
package-lock.json
go.mod
go.sum
Cargo.toml
Cargo.lock
deny.toml
pyproject.toml
poetry.lock
uv.lock
tox.ini
requirements.txt
requirements-dev.txt
constraints.txt
.pre-commit-config.yaml
Dockerfile
dev/Containerfile
docker/dev/ubuntu-26.04.Dockerfile
.github/workflows/ci.yml
changelog.d/dependencies/torch.md
DIFF
expect_exit "dependency-only path set is exempt" 0 "renovate[bot]" "renovate/all" "${work}/case1_dep_only.diff"

# Case 2: Mixed set (renovate.json + core/src/feature/vif.c) -> NOT exempt
cat >"${work}/case2_mixed.diff" <<'DIFF'
renovate.json
core/src/feature/vif.c
DIFF
expect_exit "mixed set (renovate.json + core/src/feature/vif.c) is NOT exempt" 1 "renovate[bot]" "renovate/core" "${work}/case2_mixed.diff"

# Case 3: Non-bot author with only go.mod changed -> NOT exempt
cat >"${work}/case3_nonbot.diff" <<'DIFF'
go.mod
go.sum
DIFF
expect_exit "non-bot author with only go.mod changed is NOT exempt (author/branch condition fails)" 1 "developer" "feat/update-go" "${work}/case3_nonbot.diff"

# Case 4: Dependabot bot author with dependency paths -> exempt
cat >"${work}/case4_dependabot.diff" <<'DIFF'
package.json
package-lock.json
DIFF
expect_exit "dependabot bot author is exempt" 0 "dependabot[bot]" "dependabot/npm/foo" "${work}/case4_dependabot.diff"

# Case 5: Non-bot author on renovate/* branch -> exempt
cat >"${work}/case5_branch_match.diff" <<'DIFF'
ai/pyproject.toml
DIFF
expect_exit "non-bot author on renovate/* branch is exempt" 0 "contributor" "renovate/torch-2.x" "${work}/case5_branch_match.diff"

# Case 6: Bot author editing documentation -> NOT exempt
cat >"${work}/case6_bot_docs.diff" <<'DIFF'
docs/development/automated-rule-enforcement.md
DIFF
expect_exit "bot author editing documentation is NOT exempt" 1 "renovate[bot]" "renovate/docs" "${work}/case6_bot_docs.diff"

# Case 7: Bot author editing ai/ source -> NOT exempt
cat >"${work}/case7_bot_ai.diff" <<'DIFF'
ai/pyproject.toml
ai/src/vmaf_train/cli/train.py
DIFF
expect_exit "bot author editing ai/ source is NOT exempt" 1 "renovate[bot]" "renovate/ai" "${work}/case7_bot_ai.diff"

# Case 8: Bot author editing tools/ script -> NOT exempt
cat >"${work}/case8_bot_tools.diff" <<'DIFF'
tools/vmaf-tune/tune.py
DIFF
expect_exit "bot author editing tools/ script is NOT exempt" 1 "renovate[bot]" "renovate/tools" "${work}/case8_bot_tools.diff"

# Case 9: Empty diff -> NOT exempt
touch "${work}/case9_empty.diff"
expect_exit "empty diff is NOT exempt" 1 "renovate[bot]" "renovate/empty" "${work}/case9_empty.diff"

# Case 10: Real PR #1206 fixtures (ai/pyproject.toml, mcp-server, tools) -> exempt
cat >"${work}/case10_pr1206.diff" <<'DIFF'
ai/pyproject.toml
mcp-server/vmaf-mcp/pyproject.toml
tools/ensemble-training-kit/pyproject.toml
DIFF
expect_exit "PR #1206 fixture is exempt" 0 "renovate[bot]" "renovate/torch-2.x" "${work}/case10_pr1206.diff"

# Case 11: Real PR #1207 fixtures (Dockerfiles across repo + dev/Containerfile) -> exempt
cat >"${work}/case11_pr1207.diff" <<'DIFF'
Dockerfile
Dockerfile.go-server
dev/Containerfile
docker/Dockerfile.production-gpu
docker/dev/ubuntu-26.04-cuda.Dockerfile
docker/dev/ubuntu-26.04-sycl.Dockerfile
docker/dev/ubuntu-26.04.Dockerfile
mcp-server/vmaf-mcp/Dockerfile
DIFF
expect_exit "PR #1207 fixture is exempt" 0 "renovate[bot]" "renovate/docker-dockerfile-1.x" "${work}/case11_pr1207.diff"

# Case 12: Real PR #1212 fixture -> exempt
cat >"${work}/case12_pr1212.diff" <<'DIFF'
mcp-server/vmaf-mcp/pyproject.toml
DIFF
expect_exit "PR #1212 fixture is exempt" 0 "renovate[bot]" "renovate/anyio-4.x" "${work}/case12_pr1212.diff"

# Case 13: Real PR #1214 fixture -> exempt
cat >"${work}/case13_pr1214.diff" <<'DIFF'
renovate.json
DIFF
expect_exit "PR #1214 fixture is exempt" 0 "renovate[bot]" "renovate/migrate-config" "${work}/case13_pr1214.diff"

echo ""
echo "test-classify-dependency-pr: ${pass_count} passed, ${fail_count} failed"

if [ "${fail_count}" -ne 0 ]; then
  exit 1
fi
exit 0
