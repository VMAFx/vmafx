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

# --- Helm chart / compose image-tag surfaces -------------------------------
# PR #1232 was BLOCKED by the deliverables gate because deploy/helm/*
# matched no allowlist entry, even though it is a pure image-tag bump.

# Case 14: Real PR #1232 fixture (Helm values image tag) -> exempt
cat >"${work}/case14_pr1232.diff" <<'DIFF'
deploy/helm/vmafx/values.yaml
DIFF
expect_exit "PR #1232 fixture (helm values) is exempt" 0 "app/renovate" \
  "renovate/otel-opentelemetry-collector-contrib-0.x" "${work}/case14_pr1232.diff"

# Case 15: Helm chart template carrying a pinned image -> exempt
cat >"${work}/case15_helm_template.diff" <<'DIFF'
deploy/helm/vmafx/templates/tests/test-connection.yaml
DIFF
expect_exit "helm chart template image pin is exempt" 0 "renovate[bot]" \
  "renovate/busybox-1.x" "${work}/case15_helm_template.diff"

# Case 16: Helm chart dependency pins -> exempt
cat >"${work}/case16_chart.diff" <<'DIFF'
deploy/helm/vmafx/Chart.yaml
deploy/helm/vmafx/Chart.lock
DIFF
expect_exit "helm Chart.yaml/Chart.lock are exempt" 0 "renovate[bot]" \
  "renovate/helm-deps" "${work}/case16_chart.diff"

# Case 17: compose image tags -> exempt
cat >"${work}/case17_compose.diff" <<'DIFF'
dev/docker-compose.yml
DIFF
expect_exit "docker-compose image tag is exempt" 0 "renovate[bot]" \
  "renovate/postgres-17.x" "${work}/case17_compose.diff"

# Case 18: THE ASYMMETRY. A bot PR that also edits source must STAY gated,
# otherwise widening the allowlist would have weakened the gate.
cat >"${work}/case18_helm_plus_source.diff" <<'DIFF'
deploy/helm/vmafx/values.yaml
core/src/model.c
DIFF
expect_exit "helm values + core/ source is NOT exempt" 1 "renovate[bot]" \
  "renovate/otel-0.x" "${work}/case18_helm_plus_source.diff"

# Case 19: A human PR touching the same Helm path must STAY gated.
cat >"${work}/case19_human_helm.diff" <<'DIFF'
deploy/helm/vmafx/values.yaml
DIFF
expect_exit "human-authored helm change is NOT exempt" 1 "lusoris" \
  "feat/tune-helm-defaults" "${work}/case19_human_helm.diff"

# Case 20: REGRESSION — BASE_SHA/HEAD_SHA path with a base branch that has MOVED.
#
# Every case above feeds a precomputed --diff file, which bypasses the git-diff
# code path entirely. That is exactly why the following bug survived: the
# explicit-SHA branch used a two-dot `base_sha..head_sha` range, and GitHub's
# `pull_request.base.sha` is the base tip at PR-creation time, not the current
# merge base. Once master moves, two-dot reports every file merged since the
# branch point on top of the PR's own change -- a Renovate PR touching only
# `deploy/helm/vmafx/values.yaml` was seen as touching 36 files including
# `core/src/feature/*.c`, so the classifier refused the exemption and the PR
# could never pass the documentation gates.
#
# This case builds a real repo where the base advances with a SOURCE file after
# the PR branches, then asserts the classifier still sees only the PR's own file.
repo="${work}/case20_repo"
mkdir -p "${repo}/deploy/helm/vmafx" "${repo}/core/src/feature"
(
  cd "${repo}" || exit 1
  git init -q .
  git config user.email t@example.com
  git config user.name t
  echo "image: v1" >deploy/helm/vmafx/values.yaml
  echo "int x;" >core/src/feature/seed.c
  git add -A && git commit -qm base

  # PR branches here.
  git branch pr-branch
  fork_point="$(git rev-parse HEAD)"

  # Base branch moves on, touching SOURCE -- this is what poisons a two-dot diff.
  echo "int y;" >>core/src/feature/seed.c
  git add -A && git commit -qm "master moves, touching source"
  base_sha="$(git rev-parse HEAD)"

  # The PR itself changes only the Helm values file.
  git checkout -q pr-branch
  echo "image: v2" >deploy/helm/vmafx/values.yaml
  git add -A && git commit -qm "renovate: bump image tag"
  head_sha="$(git rev-parse HEAD)"

  printf '%s %s %s\n' "${fork_point}" "${base_sha}" "${head_sha}" >"${repo}/.shas"
) >/dev/null 2>&1

read -r _fork case20_base case20_head <"${repo}/.shas"
if (cd "${repo}" && PR_AUTHOR="renovate[bot]" HEAD_REF="renovate/img-0.x" \
  BASE_SHA="${case20_base}" HEAD_SHA="${case20_head}" \
  bash "${classifier}" >/dev/null 2>&1); then
  echo "PASS: moved base branch still classifies a helm-only bot PR as exempt"
  pass_count=$((pass_count + 1))
else
  echo "FAIL: moved base branch broke the exemption (two-dot diff regression)"
  fail_count=$((fail_count + 1))
fi

echo ""
echo "test-classify-dependency-pr: ${pass_count} passed, ${fail_count} failed"

if [ "${fail_count}" -ne 0 ]; then
  exit 1
fi
exit 0
