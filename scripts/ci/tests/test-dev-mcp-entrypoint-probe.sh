#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh — shell-injection regression
# test for `_probe_with_retry` in dev/scripts/dev-mcp-entrypoint.sh.
#
# Background: the GPU visibility probe once ran its command through
# `eval "${cmd}"`. The entrypoint is PID 1 of the dev container and holds its
# whole environment, so a probe value that is ever taken from configuration
# would have been command injection. The fix runs the probe as one program
# name, argv[0], with no shell interpretation. That fix was reverted once by a
# pull request that carried a stale copy of the script, and nothing noticed,
# because no test covered it. This is that test; it is the
# `test-dev-mcp-entrypoint-probe` pre-commit hook, which the required
# Pre-Commit CI job runs with --all-files.
#
# The entrypoint cannot be sourced: it chowns, execs `tail -F` and never
# returns. The test lifts only the function out of the real script, so it
# always exercises the code that ships.
#
# Oracle: a payload creates a marker file under `mktemp -d`. The probe value
# must be treated as a program name, so no marker may ever appear.
#
# Exit codes: 0 all cases pass, 1 a case failed, 2 preflight failure.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../../.." && pwd)"
ENTRYPOINT="${REPO_ROOT}/dev/scripts/dev-mcp-entrypoint.sh"

if [[ ! -f "${ENTRYPOINT}" ]]; then
  echo "test-dev-mcp-entrypoint-probe: entrypoint not found: ${ENTRYPOINT}" >&2
  exit 2
fi

WORKDIR="$(mktemp -d)"
cleanup() {
  rm -rf -- "${WORKDIR}"
}
trap cleanup EXIT

FUNC_FILE="${WORKDIR}/probe-function.sh"
awk '/^_probe_with_retry\(\) \{$/ { inside = 1 }
     inside { print }
     inside && /^\}$/ { exit }' "${ENTRYPOINT}" >"${FUNC_FILE}"
if ! grep -q '^}$' "${FUNC_FILE}"; then
  echo "test-dev-mcp-entrypoint-probe: could not extract _probe_with_retry" >&2
  exit 2
fi
# shellcheck source=/dev/null
source "${FUNC_FILE}"

# The probe waits 3 s between its ten attempts. Count the waits instead.
SLEEPS="${WORKDIR}/sleeps"
sleep() {
  echo "$*" >>"${SLEEPS}"
}

mkdir -p "${WORKDIR}/bin"
cat >"${WORKDIR}/bin/fake-gpu-ls" <<'EOF'
#!/usr/bin/env bash
echo "[level_zero:gpu][level_zero:0] Intel(R) Arc(TM) Graphics"
EOF
cat >"${WORKDIR}/bin/fake-sycl-opencl-gpu" <<'EOF'
#!/usr/bin/env bash
echo "[opencl:gpu][opencl:1] Intel(R) Arc(TM) Graphics"
EOF
cat >"${WORKDIR}/bin/fake-hip-device-type" <<'EOF'
#!/usr/bin/env bash
echo "  Device Type:             GPU"
EOF
cat >"${WORKDIR}/bin/fake-probe-diagnostic" <<'EOF'
#!/usr/bin/env bash
echo "failed to initialise gfx1036; Device Type: GPU unavailable"
EOF
cat >"${WORKDIR}/bin/fake-silent" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "${WORKDIR}/bin/fake-gpu-ls" "${WORKDIR}/bin/fake-sycl-opencl-gpu" \
  "${WORKDIR}/bin/fake-hip-device-type" "${WORKDIR}/bin/fake-probe-diagnostic" \
  "${WORKDIR}/bin/fake-silent"
PATH="${WORKDIR}/bin:${PATH}"

pass=0
fail=0
ok() {
  echo "[test-dev-mcp-entrypoint-probe] PASS: $1"
  pass=$((pass + 1))
}
ko() {
  echo "[test-dev-mcp-entrypoint-probe] FAIL: $1" >&2
  fail=$((fail + 1))
}

# --- Case 1: a plain program name is run and its output matched. ------------
out="$(_probe_with_retry "SYCL level_zero:gpu" "fake-gpu-ls" "level_zero.*gpu" "advice")"
if grep -q 'SYCL level_zero:gpu detected (attempt 1)' <<<"${out}"; then
  ok "plain program name is probed and detected on the first attempt"
else
  ko "plain program name was not detected: ${out}"
fi

# --- Case 2: production patterns accept real GPU records only. -------------
# Read the regex literals from the real call sites so these cases exercise the
# patterns that ship rather than test-owned copies. The SYCL runtime can expose
# Arc through OpenCL without a Level Zero line; rocminfo can expose a GPU via
# its Device Type record without printing a gfx marketing name first.
extract_pattern() {
  local variable="$1"
  sed -nE "s/^readonly ${variable}='(.*)'$/\\1/p" "${ENTRYPOINT}"
}

sycl_pattern="$(extract_pattern SYCL_GPU_RECORD_PATTERN)"
hip_pattern="$(extract_pattern HIP_GPU_RECORD_PATTERN)"
if [[ -n "${sycl_pattern}" && -n "${hip_pattern}" ]]; then
  ok "production SYCL and HIP patterns were extracted"
else
  ko "could not extract production patterns: sycl='${sycl_pattern}' hip='${hip_pattern}'"
fi

out="$(_probe_with_retry "SYCL gpu" "fake-sycl-opencl-gpu" "${sycl_pattern}" "advice")"
if grep -q 'SYCL gpu detected (attempt 1)' <<<"${out}"; then
  ok "OpenCL-only SYCL GPU output is detected"
else
  ko "OpenCL-only SYCL GPU output was missed: ${out}"
fi

out="$(_probe_with_retry "HIP HSA gpu agent" "fake-hip-device-type" "${hip_pattern}" "advice")"
if grep -q 'HIP HSA gpu agent detected (attempt 1)' <<<"${out}"; then
  ok "rocminfo Device Type GPU output is detected"
else
  ko "rocminfo Device Type GPU output was missed: ${out}"
fi

: >"${SLEEPS}"
out="$(_probe_with_retry "HIP HSA gpu agent" "fake-probe-diagnostic" "${hip_pattern}" "advice")"
attempts="$(wc -l <"${SLEEPS}")"
if [[ "${attempts}" -eq 10 ]] && grep -q 'NOT detected after 10 attempts' <<<"${out}"; then
  ok "diagnostic text containing a GPU token is not a device record"
else
  ko "diagnostic text produced a false HIP detection: ${out}"
fi

# --- Cases 3-5: shell syntax in the probe value is never interpreted. -------
# Each value would create its marker under `eval`. As argv[0] it is merely a
# program that does not exist, so the probe reports a miss and carries on.
hostile() {
  local name="$1" value="$2" marker="$3"
  local rc=0
  : >"${SLEEPS}"
  out="$(_probe_with_retry "hostile ${name}" "${value}" "level_zero.*gpu" "advice")" || rc=$?
  if [[ -e "${marker}" ]]; then
    ko "${name}: payload executed (marker file created)"
    rm -f -- "${marker}"
  else
    ok "${name}: payload contained (no marker file)"
  fi
  if [[ "${rc}" -eq 0 ]] && grep -q 'NOT detected after 10 attempts' <<<"${out}"; then
    ok "${name}: reported as a miss, entrypoint keeps running (rc 0)"
  else
    ko "${name}: expected a miss with rc 0, got rc ${rc}: ${out}"
  fi
}
hostile "command separator" \
  "fake-gpu-ls; touch ${WORKDIR}/pwn-separator" "${WORKDIR}/pwn-separator"
hostile "command substitution" \
  "\$(touch ${WORKDIR}/pwn-substitution)" "${WORKDIR}/pwn-substitution"
hostile "backtick substitution" \
  "\`touch ${WORKDIR}/pwn-backtick\`" "${WORKDIR}/pwn-backtick"

# --- Case 6: a real miss retries ten times, warns, and returns 0. -----------
rc=0
: >"${SLEEPS}"
out="$(_probe_with_retry "HIP HSA agent" "fake-silent" "Agent.*GPU" "check /dev/kfd")" || rc=$?
attempts="$(wc -l <"${SLEEPS}")"
if [[ "${rc}" -eq 0 && "${attempts}" -eq 10 ]] && grep -q 'check /dev/kfd' <<<"${out}"; then
  ok "a miss makes ten attempts, prints the advice and returns 0"
else
  ko "miss path: rc=${rc} attempts=${attempts} output=${out}"
fi

# --- Case 7: no `eval` anywhere in the entrypoint. --------------------------
# Comments may mention it; a command may not. `[^#]*` cannot cross a `#`, so a
# comment line never matches.
if grep -nE '^[[:space:]]*[^#]*\beval\b' "${ENTRYPOINT}"; then
  ko "the entrypoint runs a string through eval (see the lines above)"
else
  ok "the entrypoint has no eval command"
fi

echo "[test-dev-mcp-entrypoint-probe] summary: ${pass} pass, ${fail} fail"
[[ "${fail}" -eq 0 ]]
