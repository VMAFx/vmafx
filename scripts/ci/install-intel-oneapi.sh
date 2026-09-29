#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install Intel's oneAPI DPC++/C++ compiler (builder) or its SYCL runtime
# (runtime) from Intel's apt repository, at the exact build pinned in
# build-config.env (ADR-1368).
#
# The compiler and the runtime must come from one oneAPI release: the SYCL
# soname follows the major version (libsycl.so.8 for 2025, .so.9 for 2026), so a
# binary compiled by one release does not load against another's runtime.
# Intel's container images cannot supply such a pair on the release track's
# Debian 13 (see the oneAPI block of build-config.env), so both sides install
# from Intel's apt repository instead:
#
#   builder  ${ONEAPI_APT_PACKAGE} at ONEAPI_APT_VERSION (icpx and its libsycl)
#   runtime  ${ONEAPI_RUNTIME_APT_PACKAGES} at ONEAPI_APT_VERSION, the UMF
#            packages among them at ONEAPI_UMF_APT_VERSION (libsycl, the Unified
#            Runtime adapters and the libumf.so.1 they need)
#
# The packages named on the command line pull in sub-packages with `>=`
# dependencies, which apt would satisfy with the newest build in the repository.
# An apt preference therefore pins every intel-oneapi package Intel built for
# the release to ONEAPI_APT_VERSION, and UMF (its own version line) to
# ONEAPI_UMF_APT_VERSION. The named packages' installed versions are checked
# afterwards, and the resolved closure is printed to the build log.
#
# The repository key is pinned by fingerprint: only the key named by
# INTEL_ONEAPI_APT_SIGNER_FINGERPRINT goes into the keyring apt trusts for it.
#
# Debian / Ubuntu only. Root containers run it directly; hosts run it through
# sudo.
#
# Usage: install-intel-oneapi.sh --mode=builder|runtime [repo-root]

set -euo pipefail

mode=""
repo_root=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mode=*)
      mode="${1#--mode=}"
      shift
      ;;
    --mode)
      if [ $# -lt 2 ]; then
        echo "::error::install-intel-oneapi: --mode requires a value" >&2
        exit 2
      fi
      mode="$2"
      shift 2
      ;;
    -*)
      echo "::error::install-intel-oneapi: unknown option '$1'" >&2
      exit 2
      ;;
    *)
      if [ -n "$repo_root" ]; then
        echo "::error::install-intel-oneapi: unexpected argument '$1'" >&2
        exit 2
      fi
      repo_root="$1"
      shift
      ;;
  esac
done
if [ "$mode" != builder ] && [ "$mode" != runtime ]; then
  echo "::error::install-intel-oneapi: --mode must be 'builder' or 'runtime'" >&2
  exit 2
fi

repo_root="${repo_root:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}"
config="$repo_root/build-config.env"
if [ ! -f "$config" ]; then
  echo "::error::install-intel-oneapi: $config not found" >&2
  exit 2
fi
# shellcheck disable=SC1090  # path is computed, and the file is repo-owned
. "$config"

for name in ONEAPI_VERSION ONEAPI_APT_VERSION ONEAPI_UMF_APT_VERSION ONEAPI_APT_PACKAGE \
  ONEAPI_RUNTIME_APT_PACKAGES INTEL_ONEAPI_APT_SIGNER_FINGERPRINT; do
  if [ -z "${!name:-}" ]; then
    echo "::error::install-intel-oneapi: $name is unset in build-config.env" >&2
    exit 2
  fi
done
# ONEAPI_APT_VERSION repeats the release as a latch: moving ONEAPI_VERSION
# alone fails here until the exact Intel build of the new release is recorded.
case "$ONEAPI_APT_VERSION" in
  "$ONEAPI_VERSION".*-*) ;;
  *)
    echo "::error::install-intel-oneapi: ONEAPI_APT_VERSION=$ONEAPI_APT_VERSION is not a" \
      "build of ONEAPI_VERSION=$ONEAPI_VERSION; record the exact Intel apt version" >&2
    exit 2
    ;;
esac
if [ "$ONEAPI_APT_PACKAGE" != "intel-oneapi-compiler-dpcpp-cpp-$ONEAPI_VERSION" ]; then
  echo "::error::install-intel-oneapi: ONEAPI_APT_PACKAGE=$ONEAPI_APT_PACKAGE does not name" \
    "the ONEAPI_VERSION=$ONEAPI_VERSION compiler" >&2
  exit 2
fi
if ! printf '%s' "$INTEL_ONEAPI_APT_SIGNER_FINGERPRINT" | grep -Eq '^[0-9A-F]{40}$'; then
  echo "::error::install-intel-oneapi: INTEL_ONEAPI_APT_SIGNER_FINGERPRINT is not a" \
    "40-digit upper-case fingerprint" >&2
  exit 2
fi
if ! command -v apt-get >/dev/null 2>&1; then
  echo "::error::install-intel-oneapi: needs apt-get (Debian / Ubuntu)" >&2
  exit 2
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "::error::install-intel-oneapi: non-root execution requires sudo" >&2
    exit 1
  fi
  SUDO="sudo"
fi

if ! command -v curl >/dev/null 2>&1 || ! command -v gpg >/dev/null 2>&1 ||
  ! dpkg -s ca-certificates >/dev/null 2>&1; then
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq --no-install-recommends ca-certificates curl gpg
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# --- repository key: keep exactly the pinned key --------------------------
key_url="https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB"
keyring=/usr/share/keyrings/intel-oneapi-archive-keyring.gpg
curl -fsSL --proto '=https' --tlsv1.2 -o "$tmp/intel.pub" "$key_url"
export GNUPGHOME="$tmp/gnupg"
mkdir -m 0700 "$GNUPGHOME"
# Without gpg-agent (a Recommends of gpg) the import still stores the public
# keys but exits 2; the fingerprint check below is what decides.
gpg --batch --quiet --no-autostart --import "$tmp/intel.pub" 2>/dev/null || true
gpg --batch --export "$INTEL_ONEAPI_APT_SIGNER_FINGERPRINT" >"$tmp/keyring.gpg"
exported="$(gpg --batch --with-colons --show-keys "$tmp/keyring.gpg" 2>/dev/null |
  awk -F: '$1 == "fpr" { print $10; exit }')"
if [ "$exported" != "$INTEL_ONEAPI_APT_SIGNER_FINGERPRINT" ]; then
  echo "::error::install-intel-oneapi: $key_url no longer carries the pinned key" \
    "$INTEL_ONEAPI_APT_SIGNER_FINGERPRINT; review Intel's new key and update the pin" >&2
  exit 1
fi
$SUDO install -m 0644 "$tmp/keyring.gpg" "$keyring"
echo "deb [signed-by=$keyring] https://apt.repos.intel.com/oneapi all main" |
  $SUDO tee /etc/apt/sources.list.d/intel-oneapi.list >/dev/null

# --- version pins ---------------------------------------------------------
$SUDO tee /etc/apt/preferences.d/intel-oneapi-release >/dev/null <<EOF
# build-config.env: ONEAPI_APT_VERSION, ONEAPI_UMF_APT_VERSION (ADR-1368)
Package: intel-oneapi-*
Pin: version $ONEAPI_APT_VERSION
Pin-Priority: 1001

Package: intel-oneapi-umf intel-oneapi-umf-*
Pin: version $ONEAPI_UMF_APT_VERSION
Pin-Priority: 1001
EOF
$SUDO apt-get update -qq

operands=()
case "$mode" in
  builder) operands=("$ONEAPI_APT_PACKAGE=$ONEAPI_APT_VERSION") ;;
  runtime)
    for package in $ONEAPI_RUNTIME_APT_PACKAGES; do
      case "$package" in
        intel-oneapi-umf*) operands+=("$package=$ONEAPI_UMF_APT_VERSION") ;;
        *) operands+=("$package=$ONEAPI_APT_VERSION") ;;
      esac
    done
    ;;
esac
$SUDO apt-get install -y -qq --no-install-recommends "${operands[@]}"

# --- verify ---------------------------------------------------------------
for operand in "${operands[@]}"; do
  package="${operand%%=*}"
  want="${operand#*=}"
  if ! actual="$(dpkg-query -W -f='${Version}' "$package" 2>/dev/null)" || [ "$actual" != "$want" ]; then
    echo "::error::install-intel-oneapi: $package is '${actual:-absent}', expected $want" >&2
    exit 1
  fi
done
# Record the resolved closure in the build log; the image SBOM lists it too.
dpkg-query -W -f='${db:Status-Abbrev} ${Package} ${Version}\n' 'intel-oneapi-*' 2>/dev/null |
  awk '$1 == "ii" { print "  " $2 " " $3 }'

release="${ONEAPI_APT_VERSION%%-*}"
case "$mode" in
  builder)
    icpx="/opt/intel/oneapi/compiler/$ONEAPI_VERSION/bin/icpx"
    if [ ! -x "$icpx" ]; then
      echo "::error::install-intel-oneapi: no icpx at $icpx; the package layout changed" >&2
      exit 1
    fi
    if ! "$icpx" --version | grep -q "Compiler $release "; then
      "$icpx" --version >&2
      echo "::error::install-intel-oneapi: icpx does not report oneAPI $release" >&2
      exit 1
    fi
    echo "install-intel-oneapi: oneAPI $release compiler ($ONEAPI_APT_VERSION) at $icpx"
    ;;
  runtime)
    $SUDO ldconfig
    if ! $SUDO ldconfig -p | grep -q 'libsycl\.so\.[0-9]'; then
      echo "::error::install-intel-oneapi: libsycl does not resolve after installing the runtime" >&2
      exit 1
    fi
    echo "install-intel-oneapi: oneAPI $release runtime ($ONEAPI_APT_VERSION," \
      "UMF $ONEAPI_UMF_APT_VERSION)"
    ;;
esac
