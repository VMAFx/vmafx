#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# run-coverage-python-shard.sh — run one shard of the Coverage Gate's Python
# suite with the exact selection the unsharded suite saw (issues #3073, #3501).
#
# The four "Run Python test suite, shard N/4" steps in
# .github/workflows/tests-and-quality-gates.yml repeat this one command line
# four times, and the workflow file is size-bound
# (scripts/ci/check-workflow-size.py, 64 KiB), so the repeated pytest arguments
# live here once and each workflow step calls this script with --shard I/N.
# The shard runner and the selection are unchanged from the unsharded suite:
#
#   python3 scripts/ci/run_pytest_shard.py --shard I/N -- \
#     python/test/ -v -m "not slow" --tb=short \
#     --timeout=600 --timeout-method=thread \
#     --ignore=python/test/resource \
#     --deselect python/test/command_line_test.py::CommandLineTest
#
# The outer `timeout` only keeps a wedged vmaf subprocess from eating the job
# budget before gcovr runs; it execs the shard runner so its signal still
# reaches pytest directly. Mechanics of the shards themselves (round-robin
# over collection order, the 30-minute praetor step bound that forces one
# step per shard) are documented in
# docs/development/ci-workflow-notes/tests-and-quality-gates.md and
# scripts/ci/run_pytest_shard.py.

set -euo pipefail

if [ "$#" -ne 2 ] || [ "$1" != "--shard" ]; then
  echo "usage: $0 --shard I/N (1 <= I <= N, the Coverage Gate shards 4/4)" >&2
  exit 2
fi

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)" || exit 1
cd "${REPO_ROOT}" || exit 1

exec timeout --kill-after=30s 30m python3 scripts/ci/run_pytest_shard.py \
  --shard "$2" -- \
  python/test/ -v -m "not slow" --tb=short \
  --timeout=600 --timeout-method=thread \
  --ignore=python/test/resource \
  --deselect python/test/command_line_test.py::CommandLineTest
