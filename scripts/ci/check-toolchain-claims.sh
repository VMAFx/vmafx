#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/check-toolchain-claims.sh — gate for PR-body toolchain absence claims.
#
# Enforces that any claim asserting a toolchain is missing or that a GPU backend
# was not compiled is backed by evidence from `scripts/dev/toolchain-probe.sh`.
#
# Refuses (exit 1, message naming line) when:
#   1. Body contains a missing-toolchain claim and lacks a `toolchain-probe v1` block.
#   2. Body claims a toolchain is missing/not compiled, but the pasted probe output
#      reports that toolchain was `found` on the host.
#
# Exempts lines inside fenced blocks of probe output.
#
# Reads PR body from $PR_BODY env var or stdin via scripts/ci/pr-body-input.sh.
#
# Exit codes:
#   0 — pass (claims justified by probe, or no claims made).
#   1 — claim violation (missing probe evidence or contradicted by probe).
#   2 — usage error (no body supplied).

set -euo pipefail

_check_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/ci/pr-body-input.sh
. "${_check_dir}/pr-body-input.sh"

if [ -n "${PR_BODY+x}" ]; then
  body_text="${PR_BODY}"
else
  pr_body_classify_stdin
  if [ "${PR_BODY_STDIN_KIND}" = "stream" ]; then
    body_text="$(pr_body_read_stdin)"
    pr_body_close_stdin
  else
    echo "check-toolchain-claims: no PR body supplied — $(pr_body_stdin_reason)" >&2
    echo "  Set PR_BODY env var, OR pipe body on stdin." >&2
    exit 2
  fi
fi

# Empty or blank body: no claims to evaluate.
if [ -z "$(printf '%s' "${body_text}" | tr -d '[:space:]')" ]; then
  exit 0
fi

# Split body into indexed lines
declare -a lines=()
while IFS= read -r line || [ -n "${line}" ]; do
  lines+=("${line}")
done <<<"${body_text}"

total_lines="${#lines[@]}"
if [ "${total_lines}" -eq 0 ]; then
  exit 0
fi

# Pass 1: Scan for probe header, probe findings, and probe code block boundaries
has_probe_v1=0
has_probe_results=0
found_tools=" "
declare -a exempt_line_map=()

tool_is_found() {
  case " ${found_tools} " in
    *" $1 "*) return 0 ;;
    *) return 1 ;;
  esac
}

in_fence=0
fence_char=""
fence_len=0
fence_has_probe=0
declare -a current_fence_lines=()

shopt -s nocasematch

idx=0
while [ "${idx}" -lt "${total_lines}" ]; do
  idx=$((idx + 1))
  cur_line="${lines[$((idx - 1))]}"

  # Check if line contains probe v1 header
  if [[ "${cur_line}" =~ (^|[[:space:]])toolchain-probe[[:space:]]+v1[[:space:]]+host= ]] ||
    [[ "${cur_line}" =~ ^[[:space:]]*toolchain-probe[[:space:]]+v1([[:space:]]|$) ]]; then
    has_probe_v1=1
  fi

  # Record toolchains the probe reported (both found and absent count as results)
  if [[ "${cur_line}" =~ ^[[:space:]]*toolchain-probe:[[:space:]]*([a-zA-Z0-9_-]+)[[:space:]]+(found|absent)([[:space:]]|$) ]]; then
    has_probe_results=1
    tool_name="${BASH_REMATCH[1]}"
    result_status="${BASH_REMATCH[2]}"
    if [ "${result_status}" = "found" ]; then
      tool_name_lower="$(printf '%s' "${tool_name}" | tr '[:upper:]' '[:lower:]')"
      found_tools="${found_tools}${tool_name_lower} "
    fi
  fi

  # Track markdown fenced blocks
  if [ "${in_fence}" -eq 0 ]; then
    if [[ "${cur_line}" =~ ^[[:space:]]*(\`{3,}|~{3,}) ]]; then
      in_fence=1
      fence_char="${BASH_REMATCH[1]:0:1}"
      fence_len="${#BASH_REMATCH[1]}"
      fence_has_probe=0
      current_fence_lines=("${idx}")
    fi
  else
    close_matched=0
    if [ "${fence_char}" = '`' ]; then
      if [[ "${cur_line}" =~ ^[[:space:]]*\`{${fence_len},}[[:space:]]*$ ]]; then
        close_matched=1
      fi
    else
      if [[ "${cur_line}" =~ ^[[:space:]]*~{${fence_len},}[[:space:]]*$ ]]; then
        close_matched=1
      fi
    fi

    if [ "${close_matched}" -eq 1 ]; then
      in_fence=0
      current_fence_lines+=("${idx}")
      if [ "${fence_has_probe}" -eq 1 ]; then
        for f_line in "${current_fence_lines[@]}"; do
          exempt_line_map[f_line]=1
        done
      fi
      current_fence_lines=()
      fence_char=""
      fence_len=0
    else
      current_fence_lines+=("${idx}")
      if [[ "${cur_line}" =~ toolchain-probe ]]; then
        fence_has_probe=1
      fi
    fi
  fi

  # Any raw probe output line is also exempt from claim checking
  if [[ "${cur_line}" =~ ^[[:space:]]*toolchain-probe ]]; then
    exempt_line_map[idx]=1
  fi
done

# If a fence block stayed open and had probe output, exempt it
if [ "${in_fence}" -eq 1 ] && [ "${fence_has_probe}" -eq 1 ]; then
  for f_line in "${current_fence_lines[@]}"; do
    exempt_line_map[f_line]=1
  done
fi

# Missing toolchain claim regexes
p_no="(^|[^a-zA-Z0-9_])no[[:space:]]+\`?(icpx|icx|nvcc|hipcc|hip[[:space:]]?cc|oneapi|cuda[[:space:]]+toolkit|rocm|glslc)\`?([^a-zA-Z0-9_]|$)"
p_status="(^|[^a-zA-Z0-9_])\`?(icpx|icx|nvcc|hipcc|oneapi|rocm|cuda)\`?([[:space:]]+is)?([[:space:]]+)(not[[:space:]]+installed|missing|unavailable|absent)([^a-zA-Z0-9_]|$)"
p_without="(^|[^a-zA-Z0-9_])without[[:space:]]+\`?(icpx|icx|nvcc|hipcc)\`?([[:space:]]+on[[:space:]]+this[[:space:]]+host)?([^a-zA-Z0-9_]|$)"
p_compiled_was="(^|[^a-zA-Z0-9_])(was|were)[[:space:]]+not[[:space:]]+compiled([^a-zA-Z0-9_]|$)"
p_compiled_not="(^|[^a-zA-Z0-9_])not[[:space:]]+compiled([[:space:]]+(locally|here|on[[:space:]]+this[[:space:]]+host))?([^a-zA-Z0-9_]|$)"

fail=0

# Pass 2: Check non-exempt lines for claims
idx=0
while [ "${idx}" -lt "${total_lines}" ]; do
  idx=$((idx + 1))
  cur_line="${lines[$((idx - 1))]}"

  # Skip exempted lines (probe output blocks)
  if [ "${exempt_line_map[idx]:-0}" -eq 1 ]; then
    continue
  fi

  is_claim=0
  names_host=0
  if [[ "${cur_line}" =~ ${p_no} ]] ||
    [[ "${cur_line}" =~ ${p_status} ]] ||
    [[ "${cur_line}" =~ ${p_without} ]]; then
    is_claim=1
    names_host=1
  elif [[ "${cur_line}" =~ ${p_compiled_was} ]] ||
    [[ "${cur_line}" =~ ${p_compiled_not} ]]; then
    is_claim=1
    # A bare "not compiled" can describe a build configuration (the golden
    # build has no GPU backend); it contradicts the probe only when it is
    # about this host.
    if [[ "${cur_line}" =~ (this[[:space:]]+host|locally|on[[:space:]]+the[[:space:]]+host|here) ]]; then
      names_host=1
    fi
  fi

  if [ "${is_claim}" -eq 0 ]; then
    continue
  fi

  # Condition 1: Claim made without toolchain-probe v1 block
  if [ "${has_probe_v1}" -eq 0 ]; then
    echo "::error file=PR_BODY,line=${idx},title=Missing toolchain probe::line ${idx} makes a missing-toolchain claim without a 'toolchain-probe v1' block in PR body:" >&2
    echo "check-toolchain-claims: line ${idx}: missing-toolchain claim without 'toolchain-probe v1' block in PR body:" >&2
    echo "  ${cur_line}" >&2
    echo "  Hint: run scripts/dev/toolchain-probe.sh on the build host and paste its fenced output into the PR description." >&2
    fail=1
    continue
  fi

  # Condition 1b: Claim made with probe header but no probe results
  if [ "${has_probe_results}" -eq 0 ]; then
    echo "::error file=PR_BODY,line=${idx},title=Missing toolchain probe results::line ${idx} makes a missing-toolchain claim with 'toolchain-probe v1' header but no toolchain result lines:" >&2
    echo "check-toolchain-claims: line ${idx}: missing-toolchain claim with 'toolchain-probe v1' header but no toolchain result lines:" >&2
    echo "  ${cur_line}" >&2
    echo "  Hint: paste complete output from scripts/dev/toolchain-probe.sh into the PR description." >&2
    fail=1
    continue
  fi

  # Condition 2: Claim contradicts probe (probe says found, body claims absent/not compiled)
  line_contradicts=0
  contradicted_tool=""
  if [ "${names_host}" -eq 0 ]; then
    continue
  fi

  # Check specific tool mentions
  if [[ "${cur_line}" =~ icpx ]] && tool_is_found "icpx"; then
    line_contradicts=1
    contradicted_tool="icpx"
  elif [[ "${cur_line}" =~ icx ]] && tool_is_found "icx"; then
    line_contradicts=1
    contradicted_tool="icx"
  elif [[ "${cur_line}" =~ (sycl|oneapi) ]] && { tool_is_found "icpx" || tool_is_found "icx"; }; then
    line_contradicts=1
    contradicted_tool="oneapi / icpx"
  elif [[ "${cur_line}" =~ (nvcc|cuda) ]] && tool_is_found "nvcc"; then
    line_contradicts=1
    contradicted_tool="nvcc"
  elif [[ "${cur_line}" =~ (hipcc|hip|rocm) ]] && tool_is_found "hipcc"; then
    line_contradicts=1
    contradicted_tool="hipcc"
  elif [[ "${cur_line}" =~ (glslc|vulkan) ]] && tool_is_found "glslc"; then
    line_contradicts=1
    contradicted_tool="glslc"
  elif [[ "${cur_line}" =~ glslangvalidator ]] && tool_is_found "glslangvalidator"; then
    line_contradicts=1
    contradicted_tool="glslangValidator"
  elif [[ "${cur_line}" =~ metal ]] && tool_is_found "metal"; then
    line_contradicts=1
    contradicted_tool="metal"
  fi

  if [ "${line_contradicts}" -eq 1 ]; then
    echo "::error file=PR_BODY,line=${idx},title=Contradicted toolchain claim::line ${idx} claims '${contradicted_tool}' is missing/not compiled, but probe reports it found on host:" >&2
    echo "check-toolchain-claims: line ${idx}: PR body claims '${contradicted_tool}' is missing/not compiled, but probe reports it found on host:" >&2
    echo "  ${cur_line}" >&2
    fail=1
  fi
done

shopt -u nocasematch

if [ "${fail}" -ne 0 ]; then
  exit 1
fi

echo "check-toolchain-claims: PASS — toolchain claims verified against probe."
exit 0
