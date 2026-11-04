#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/test-check-toolchain-claims.sh — test suite for PR-body toolchain
# claim gate (scripts/ci/check-toolchain-claims.sh) and host probe (scripts/dev/toolchain-probe.sh).
#
# Follows the test pattern of scripts/ci/test-validate-pr-body.sh:
#   - Positive: claim + probe saying absent -> pass (exit 0)
#   - Negative planted: claim without probe -> fail (exit 1)
#   - Negative planted: claim with probe saying found -> fail (exit 1)
#   - Boundary: no claim -> pass (exit 0)
#   - Boundary: claim inside probe fenced block -> pass (exit 0)
#   - Probe stub test: fake setvars.sh exporting PATH to stub icpx -> found
#   - Probe empty prefix test: empty prefix -> absent

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
gate="${repo_root}/scripts/ci/check-toolchain-claims.sh"
probe="${repo_root}/scripts/dev/toolchain-probe.sh"

if [ ! -x "${gate}" ]; then
  echo "test-check-toolchain-claims: gate script missing or non-executable: ${gate}" >&2
  exit 1
fi

if [ ! -x "${probe}" ]; then
  echo "test-check-toolchain-claims: probe script missing or non-executable: ${probe}" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

pass_count=0
fail_count=0

expect_gate_exit() {
  local name="$1" expected="$2" body_file="$3"
  local out exit_code=0

  out="$(PR_BODY="$(cat "${body_file}")" "${gate}" 2>&1)" || exit_code=$?
  if [ "${exit_code}" -eq "${expected}" ]; then
    echo "PASS: ${name} via PR_BODY (exit=${exit_code})"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} via PR_BODY (got exit=${exit_code}, expected ${expected})"
    echo "----- captured output -----"
    printf '%s\n' "${out}"
    echo "---------------------------"
    fail_count=$((fail_count + 1))
  fi

  # Also test stdin pipe
  local out_stdin exit_code_stdin=0
  out_stdin="$("${gate}" <"${body_file}" 2>&1)" || exit_code_stdin=$?
  if [ "${exit_code_stdin}" -eq "${expected}" ]; then
    echo "PASS: ${name} via stdin (exit=${exit_code_stdin})"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} via stdin (got exit=${exit_code_stdin}, expected ${expected})"
    echo "----- captured output -----"
    printf '%s\n' "${out_stdin}"
    echo "---------------------------"
    fail_count=$((fail_count + 1))
  fi
}

# ---------- Case 1: Negative planted — claim without probe -> fail (exit 1) ----------
cat >"${work}/case1_no_probe.md" <<'EOF'
## Summary

SYCL was not compiled: this host has no `icpx`.
EOF
expect_gate_exit "planted: claim without probe -> exit 1" 1 "${work}/case1_no_probe.md"

# ---------- Case 2: Negative planted — claim with probe saying found -> fail (exit 1) ----------
cat >"${work}/case2_probe_found.md" <<'EOF'
## Summary

SYCL was not compiled: this host has no `icpx`.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: icpx found /opt/intel/oneapi/compiler/2026.0/bin/icpx Intel(R) oneAPI DPC++/C++ Compiler 2026.0
toolchain-probe: nvcc absent searched=PATH,/opt/cuda
```
EOF
expect_gate_exit "planted: claim with probe saying found -> exit 1" 1 "${work}/case2_probe_found.md"

# ---------- Case 3: Positive — claim + probe saying absent -> pass (exit 0) ----------
cat >"${work}/case3_probe_absent.md" <<'EOF'
## Summary

SYCL was not compiled: this host has no `icpx`.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: icpx absent searched=/opt/intel/oneapi/setvars.sh
toolchain-probe: nvcc found /opt/cuda/bin/nvcc nvcc: NVIDIA (R) Cuda compiler driver
```
EOF
expect_gate_exit "positive: claim + probe saying absent -> exit 0" 0 "${work}/case3_probe_absent.md"

# ---------- Case 4: Boundary — no claim made -> pass (exit 0) ----------
cat >"${work}/case4_clean.md" <<'EOF'
## Summary

Refactor AVX2 feature extractors for improved throughput.
All unit and regression tests pass.
EOF
expect_gate_exit "boundary: clean body with no claim -> exit 0" 0 "${work}/case4_clean.md"

# ---------- Case 5: Boundary — claim only inside probe fenced block -> pass (exit 0) ----------
cat >"${work}/case5_probe_only.md" <<'EOF'
## Summary

Update build flags.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: icpx absent searched=/opt/intel/oneapi/setvars.sh
toolchain-probe: nvcc found /opt/cuda/bin/nvcc nvcc: NVIDIA (R) Cuda compiler driver
```
EOF
expect_gate_exit "boundary: claim inside probe fenced block only -> exit 0" 0 "${work}/case5_probe_only.md"

# ---------- Case 6: CUDA claim without probe vs with probe ----------
cat >"${work}/case6_cuda_no_probe.md" <<'EOF'
## Summary
CUDA was not compiled locally: no nvcc found.
EOF
expect_gate_exit "cuda claim without probe -> exit 1" 1 "${work}/case6_cuda_no_probe.md"

cat >"${work}/case6_cuda_absent.md" <<'EOF'
## Summary
CUDA was not compiled locally: no nvcc found.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: nvcc absent searched=PATH,/opt/cuda,/usr/local/cuda
```
EOF
expect_gate_exit "cuda claim with nvcc absent -> exit 0" 0 "${work}/case6_cuda_absent.md"

cat >"${work}/case6_cuda_found.md" <<'EOF'
## Summary
CUDA was not compiled locally: no nvcc found.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: nvcc found /opt/cuda/bin/nvcc nvcc: NVIDIA (R) Cuda compiler driver
```
EOF
expect_gate_exit "cuda claim with nvcc found -> exit 1" 1 "${work}/case6_cuda_found.md"

# ---------- Case 9: build-configuration "not compiled" is no host claim ----------
cat >"${work}/case9_golden.md" <<'EOF'
## Summary
Netflix golden gate: the golden build has no CUDA backend: the changed `.c` files are not compiled.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: nvcc found /opt/cuda/bin/nvcc nvcc: NVIDIA (R) Cuda compiler driver
```
EOF
expect_gate_exit "boundary: golden-build not compiled + nvcc found -> exit 0" 0 "${work}/case9_golden.md"

cat >"${work}/case9_golden_no_probe.md" <<'EOF'
## Summary
The golden build has no CUDA backend: the changed `.c` files are not compiled.
EOF
expect_gate_exit "negative: not compiled without probe -> exit 1" 1 "${work}/case9_golden_no_probe.md"

# ---------- Case 7: ROCm / HIP claim with hipcc found vs absent ----------
cat >"${work}/case7_hip_absent.md" <<'EOF'
## Summary
HIP backend was not compiled: hipcc is missing.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: hipcc absent searched=PATH,/opt/rocm
```
EOF
expect_gate_exit "hip claim with hipcc absent -> exit 0" 0 "${work}/case7_hip_absent.md"

cat >"${work}/case7_hip_found.md" <<'EOF'
## Summary
HIP backend was not compiled: hipcc is missing.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: hipcc found /opt/rocm/bin/hipcc HIP version: 7.2.0
```
EOF
expect_gate_exit "hip claim with hipcc found -> exit 1" 1 "${work}/case7_hip_found.md"

# ---------- Case 8: Vulkan glslc claim with glslc found vs absent ----------
cat >"${work}/case8_glslc_absent.md" <<'EOF'
## Summary
Vulkan was not compiled: no glslc.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: glslc absent searched=PATH
```
EOF
expect_gate_exit "glslc claim with glslc absent -> exit 0" 0 "${work}/case8_glslc_absent.md"

cat >"${work}/case8_glslc_found.md" <<'EOF'
## Summary
Vulkan was not compiled: no glslc.

```text
toolchain-probe v1 host=test-box os=Linux date=2026-11-04T00:00:00Z
toolchain-probe: glslc found /usr/bin/glslc 2026.1
```
EOF
expect_gate_exit "glslc claim with glslc found -> exit 1" 1 "${work}/case8_glslc_found.md"

# ---------- Case 9: toolchain-probe.sh stub dir test (fake setvars.sh exporting PATH to stub icpx) ----------
stub_dir="${work}/oneapi_stub"
mkdir -p "${stub_dir}/bin"
cat >"${stub_dir}/bin/icpx" <<'EOF'
#!/bin/sh
echo "Intel(R) oneAPI DPC++/C++ Compiler 2026.9.9 (stub)"
EOF
chmod +x "${stub_dir}/bin/icpx"

cat >"${stub_dir}/setvars.sh" <<EOF
export PATH="${stub_dir}/bin:\$PATH"
EOF

probe_stub_out="$("${probe}" --prefix "${stub_dir}" 2>&1)" || {
  echo "FAIL: probe with stub dir returned non-zero exit"
  fail_count=$((fail_count + 1))
}

if printf '%s\n' "${probe_stub_out}" | grep -q "toolchain-probe: icpx found .*${stub_dir}/bin/icpx"; then
  echo "PASS: probe with stub setvars.sh reported icpx found"
  pass_count=$((pass_count + 1))
else
  echo "FAIL: probe with stub setvars.sh did not report icpx found"
  echo "----- captured output -----"
  printf '%s\n' "${probe_stub_out}"
  echo "---------------------------"
  fail_count=$((fail_count + 1))
fi

# ---------- Case 10: toolchain-probe.sh empty prefix test -> absent ----------
empty_dir="${work}/empty_prefix"
mkdir -p "${empty_dir}"

probe_empty_out="$("${probe}" --prefix "${empty_dir}" 2>&1)" || {
  echo "FAIL: probe with empty prefix returned non-zero exit"
  fail_count=$((fail_count + 1))
}

if printf '%s\n' "${probe_empty_out}" | grep -q "toolchain-probe: icpx absent searched=${empty_dir}"; then
  echo "PASS: probe with empty prefix reported icpx absent"
  pass_count=$((pass_count + 1))
else
  echo "FAIL: probe with empty prefix did not report icpx absent"
  echo "----- captured output -----"
  printf '%s\n' "${probe_empty_out}"
  echo "---------------------------"
  fail_count=$((fail_count + 1))
fi

# ---------- Case 11: toolchain-probe.sh usage error -> exit 2 ----------
probe_usage_exit=0
"${probe}" --invalid-arg >/dev/null 2>&1 || probe_usage_exit=$?
if [ "${probe_usage_exit}" -eq 2 ]; then
  echo "PASS: probe with invalid argument returned exit 2"
  pass_count=$((pass_count + 1))
else
  echo "FAIL: probe with invalid argument returned exit ${probe_usage_exit} (expected 2)"
  fail_count=$((fail_count + 1))
fi

# ---------- Case 12: deliverables-check.sh integration test ----------
deliv_script="${repo_root}/scripts/ci/deliverables-check.sh"
deliv_exit=0
# The body fails other deliverables checks too, so the exit status alone does
# not prove the wiring: require the claims gate's own finding in the output.
deliv_out="$(PR_BODY="$(cat "${work}/case1_no_probe.md")" BASE_SHA="HEAD~1" HEAD_SHA="HEAD" \
  bash "${deliv_script}" 2>&1)" || deliv_exit=$?
if [ "${deliv_exit}" -eq 1 ] &&
  [[ "${deliv_out}" == *"check-toolchain-claims: line "* ]]; then
  echo "PASS: deliverables-check.sh propagates toolchain claim gate failure (exit=1)"
  pass_count=$((pass_count + 1))
else
  echo "FAIL: deliverables-check.sh did not propagate failure (exit=${deliv_exit})"
  fail_count=$((fail_count + 1))
fi

echo ""
echo "test-check-toolchain-claims: ${pass_count} passed, ${fail_count} failed"

if [ "${fail_count}" -ne 0 ]; then
  exit 1
fi

exit 0
