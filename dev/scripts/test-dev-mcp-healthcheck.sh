#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris

# Hermetic contract test for the dev-MCP Compose healthcheck. The production
# helper accepts a device path only so this test can model NVIDIA and non-NVIDIA
# hosts without creating device nodes.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
HEALTHCHECK="${SCRIPT_DIR}/dev-mcp-healthcheck.sh"
COMPOSE_FILE="${SCRIPT_DIR}/../docker-compose.yml"

WORKDIR="$(mktemp -d)"
cleanup() {
  rm -rf -- "${WORKDIR}"
}
trap cleanup EXIT

if [[ ! -x "${HEALTHCHECK}" ]]; then
  echo "test-dev-mcp-healthcheck: executable not found: ${HEALTHCHECK}" >&2
  exit 1
fi

mkdir -p "${WORKDIR}/bin"
cat >"${WORKDIR}/bin/vmaf" <<'EOF'
#!/usr/bin/env bash
[[ "$#" -eq 1 && "$1" == "--version" ]] || exit 64
exit "${VMAFX_TEST_VMAF_RC:-0}"
EOF
cat >"${WORKDIR}/bin/nvidia-smi" <<'EOF'
#!/usr/bin/env bash
[[ "$#" -eq 2 && "$1" == "--query-gpu=index" && "$2" == "--format=csv,noheader" ]] || exit 64
exit "${VMAFX_TEST_NVIDIA_SMI_RC:-0}"
EOF
chmod +x "${WORKDIR}/bin/vmaf" "${WORKDIR}/bin/nvidia-smi"
PATH="${WORKDIR}/bin:${PATH}"

missing_device="${WORKDIR}/no-nvidia-device"
VMAFX_TEST_NVIDIA_SMI_RC=1 "${HEALTHCHECK}" "${missing_device}"

if VMAFX_TEST_NVIDIA_SMI_RC=1 "${HEALTHCHECK}" /dev/null; then
  echo "test-dev-mcp-healthcheck: NVIDIA driver failure was accepted" >&2
  exit 1
fi

VMAFX_TEST_NVIDIA_SMI_RC=0 "${HEALTHCHECK}" /dev/null

if VMAFX_TEST_VMAF_RC=1 "${HEALTHCHECK}" "${missing_device}"; then
  echo "test-dev-mcp-healthcheck: vmaf CLI failure was accepted" >&2
  exit 1
fi

health_block="$(awk '
  $0 == "  dev-mcp:" { service = 1; next }
  service && /^  [^ ]/ { exit }
  service && $0 == "    healthcheck:" { health = 1 }
  health { print }
' "${COMPOSE_FILE}")"

if ! grep -Fqx '      test: ["CMD", "/build/vmaf/dev/scripts/dev-mcp-healthcheck.sh"]' \
  <<<"${health_block}"; then
  echo "test-dev-mcp-healthcheck: Compose does not invoke the health helper" >&2
  exit 1
fi
if ! grep -Fqx '      start_period: 45s' <<<"${health_block}"; then
  echo "test-dev-mcp-healthcheck: Compose start_period is not 45s" >&2
  exit 1
fi

echo "test-dev-mcp-healthcheck: PASS"
