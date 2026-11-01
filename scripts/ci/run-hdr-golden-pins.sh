#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Run the HDR model pins of the Netflix golden job and fail unless both
# required test files actually ran (an all-skipped run is not a pass).
#
# Usage: run-hdr-golden-pins.sh <junit-xml-path>
# Kept out of tests-and-quality-gates.yml so that file stays under the
# 64 KiB verification input bound (scripts/ci/check-workflow-size.py).
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <junit-xml-path>" >&2
  exit 2
fi
junit="$1"

timeout --kill-after=30s 14m python3 -m pytest \
  python/test/hdr_models_pin_test.py \
  python/test/vmaf_v1_quality_runner_test.py \
  -v --tb=short --timeout=780 --timeout-method=signal \
  --junitxml="$junit"
python3 scripts/ci/check_required_tests_ran.py \
  --junit "$junit" \
  --required-file "hdr_models_pin_test" \
  --required-file "vmaf_v1_quality_runner_test"
