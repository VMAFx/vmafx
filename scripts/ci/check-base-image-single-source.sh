#!/usr/bin/env bash
# Enforce that every container base image is defined in exactly one place.
#
# The authoritative definition is build-config.env at the repo root. Dockerfiles
# do not name a base image directly: each takes it as a build argument whose
# default mirrors the config, so a plain `docker build` still works while
# `scripts/build/image.sh` (and CI) can override every base from the config in
# one shot.
#
# That mirror is the thing that rots, so this gate checks it, in the same shape
# scripts/ci/check-default-model-single-source.sh applies to the default model:
# one authoritative value, mirrors permitted, drift fatal.
#
# Three rules:
#   1. No Dockerfile in scope may FROM a literal registry reference. It must
#      FROM ${SOME_ARG} or from an earlier stage in the same file.
#   2. Any `ARG NAME=...` whose NAME is a key in build-config.env must carry
#      exactly the config's value.
#   3. Every pinned image tag must actually carry the version its knob claims,
#      so RELEASE_DEBIAN=13 cannot sit above a debian:12 pin.
#
# Rule 3 is what would have caught the drift that prompted this file:
# Dockerfile.controller and Dockerfile.operator were still on Debian 12 and
# distroless-debian12 long after the rest of the tree moved to Debian 13.
#
# Usage: check-base-image-single-source.sh [--write]
#   --write   rewrite Dockerfile ARG defaults from build-config.env instead of
#             failing on drift (rule 2 only; rules 1 and 3 still report)
# Exit: 0 clean; 1 a violation; 2 the config is missing or malformed.
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

config=build-config.env
write=0
[ "${1:-}" = "--write" ] && write=1

if [ ! -f "$config" ]; then
  echo "error: $config not found; it is the single source of truth for base images" >&2
  exit 2
fi

fail=0
note() { printf '%s\n' "$*" >&2; }
bad() {
  note "::error title=base-image drift::$*"
  fail=1
}

# shellcheck disable=SC1090
. "./$config"

# Dockerfiles in scope. docker/dev/ is deliberately excluded: those files pin
# alpine / arch / fedora on purpose to prove the build survives distros the
# release track does not use, so unifying their bases would defeat them.
mapfile -t files < <(
  git ls-files 'Dockerfile*' 'docker/Dockerfile*' 'dev/Containerfile*' |
    grep -v '^docker/dev/' | sort || true
)
if [ "${#files[@]}" -eq 0 ]; then
  bad "no Dockerfiles found in scope; the glob or the layout changed"
  exit 1
fi

# Config keys that name a container image, detected by the SHAPE of the value
# rather than by a list of names. An image reference always carries a registry
# path or a tag separator; a bare version ("7.2.4", "2026.1.1-325") carries
# neither, and a URL is excluded explicitly. A denylist of version-knob names
# would need editing every time a knob is added, and would silently mis-classify
# the one somebody forgot.
mapfile -t image_keys < <(
  while IFS= read -r key; do
    val="${!key-}"
    case "$val" in
      http*) continue ;;
      */* | *:*) printf '%s\n' "$key" ;;
    esac
  done < <(grep -oE '^[A-Z][A-Z0-9_]*=' "$config" | tr -d '=' | sort -u)
)

# ------------------------------------------------- rule 1: no literal FROMs --
for f in "${files[@]}"; do
  # Stage names declared in this file are legitimate FROM targets.
  stages=$(grep -oiE '^[[:space:]]*FROM[[:space:]]+\S+[[:space:]]+AS[[:space:]]+\S+' "$f" |
    awk '{print tolower($NF)}' | sort -u || true)
  while IFS=: read -r lineno line; do
    ref=$(printf '%s' "$line" | sed -E 's/^[[:space:]]*[Ff][Rr][Oo][Mm][[:space:]]+//; s/[[:space:]]+([Aa][Ss][[:space:]]+.*)?$//' | awk '{print $1}')
    # An ARG reference is exactly what we want.
    # shellcheck disable=SC2016  # matching a literal '${' prefix, not expanding it
    case "$ref" in
      '${'* | '$'*) continue ;;
      scratch) continue ;;
      # Only digest-pinned references are external bases. A bare local tag such
      # as `vmaf:latest` names an image this repo builds itself, which is not a
      # base pin and has nothing to centralise.
      *@sha256:*) ;;
      *) continue ;;
    esac
    # A reference to an earlier stage in the same file is fine.
    if printf '%s\n' "$stages" | grep -qxF "$(printf '%s' "$ref" | tr '[:upper:]' '[:lower:]')"; then
      continue
    fi
    bad "$f:$lineno hardcodes a base image: $ref"
    note "       Take it from $config instead:"
    note "         ARG SOME_BASE=\"$ref\""
    note "         FROM \${SOME_BASE} AS ..."
  done < <(grep -niE '^[[:space:]]*FROM[[:space:]]' "$f" | sed 's/^\([0-9]*\):/\1:/')
done

# `COPY --from=<image>` pulls a base in just as much as FROM does, and it is
# easier to miss: dev/Containerfile lifted the Go toolchain out of a
# golang:1.27-bookworm image this way and stayed on Debian 12 long after every
# FROM had moved to 13.
for f in "${files[@]}"; do
  while IFS=: read -r lineno line; do
    ref=$(printf '%s' "$line" | grep -oE -- '--from=[^ ]+' | head -1 | sed 's/^--from=//')
    case "$ref" in
      *@sha256:*) ;;
      *) continue ;;
    esac
    # shellcheck disable=SC2016  # matching a literal '${' prefix, not expanding it
    case "$ref" in
      '${'*) continue ;;
    esac
    bad "$f:$lineno COPY --from hardcodes a base image: $ref"
    note "       Take it from $config the same way FROM does."
  done < <(grep -nE '^[[:space:]]*COPY[[:space:]]+--from=' "$f")
done

# ------------------------------------- rule 2: ARG defaults mirror the config --
for f in "${files[@]}"; do
  for key in "${image_keys[@]}"; do
    want="${!key:-}"
    [ -z "$want" ] && continue
    # Every ARG line in this file declaring that key.
    while IFS=: read -r lineno line; do
      got=$(printf '%s' "$line" | sed -E "s/^[[:space:]]*ARG[[:space:]]+${key}=//; s/^\"//; s/\"$//")
      [ "$got" = "$want" ] && continue
      if [ "$write" -eq 1 ]; then
        python3 - "$f" "$lineno" "$key" "$want" <<'PY'
import sys
path, lineno, key, want = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
lines = open(path).read().splitlines(keepends=True)
indent = lines[lineno - 1][: len(lines[lineno - 1]) - len(lines[lineno - 1].lstrip())]
lines[lineno - 1] = f'{indent}ARG {key}="{want}"\n'
open(path, "w").writelines(lines)
PY
        note "rewrote $f:$lineno $key"
      else
        bad "$f:$lineno ARG $key drifted from $config"
        note "         config: $want"
        note "         file:   $got"
        note "       Run: scripts/ci/check-base-image-single-source.sh --write"
      fi
    done < <(grep -nE "^[[:space:]]*ARG[[:space:]]+${key}=" "$f")
  done
done

# ------------------------------------ rule 3: pins carry the claimed version --
expect_in() {
  local key="$1" needle="$2" why="$3" val="${!1:-}"
  [ -z "$val" ] && return 0
  case "$val" in
    *"$needle"*) return 0 ;;
  esac
  bad "$key does not carry $needle ($why)"
  note "         $key=$val"
}
expect_in RELEASE_BUILDER_BASE "debian:${RELEASE_DEBIAN}-" "RELEASE_DEBIAN=$RELEASE_DEBIAN"
expect_in RELEASE_RUNTIME_CC "-debian${RELEASE_DEBIAN}:" "RELEASE_DEBIAN=$RELEASE_DEBIAN"
expect_in RELEASE_RUNTIME_STATIC "-debian${RELEASE_DEBIAN}:" "RELEASE_DEBIAN=$RELEASE_DEBIAN"
expect_in RELEASE_GO_BASE "golang:${GO_VERSION}-" "GO_VERSION=$GO_VERSION"
expect_in DEV_GO_BASE "golang:${GO_VERSION}-" "GO_VERSION=$GO_VERSION"
expect_in RELEASE_PYTHON_BASE "python:${PYTHON_VERSION}-" "PYTHON_VERSION=$PYTHON_VERSION"
expect_in DEV_BASE "ubuntu:${DEV_UBUNTU}@" "DEV_UBUNTU=$DEV_UBUNTU"
expect_in CUDA_BUILDER ":${CUDA_VERSION}-" "CUDA_VERSION=$CUDA_VERSION"
expect_in CUDA_RUNTIME ":${CUDA_VERSION}-" "CUDA_VERSION=$CUDA_VERSION"
expect_in ROCM_BUILDER ":${ROCM_VERSION}" "ROCM_VERSION=$ROCM_VERSION"
expect_in ROCM_RUNTIME ":${ROCM_VERSION}" "ROCM_VERSION=$ROCM_VERSION"

# No release image may sit on a distro the release track has moved off.
#
# ONEAPI_BUILDER / ONEAPI_RUNTIME are exempt for exactly one PR. Crossing them
# to 2026 is a restructure rather than a pin swap -- the SYCL soname goes
# .so.8 -> .so.9 so both sides must move together, Intel's 2026 images are
# Ubuntu-only and their glibc is newer than Debian 13's, and Intel's runtime
# image carries the NEO GPU driver that Debian does not package. The follow-up
# moves both to Intel's apt repo on Debian 13 with NEO from
# dev/scripts/fetch-intel-neo.py, and deletes this exemption. See the oneAPI
# block in build-config.env for the measurements.
#
# ROCM_BUILDER / ROCM_RUNTIME are exempt for the same reason and with the same
# deadline: PR #1386 carries the 7.2.4 -> 10.0.0 migration together with the
# HIP-side changes it needs. rocm/dev-ubuntu-26.04:10.0.0-full is the target.
# Moving the pin here without that PR's code would bump a major GPU SDK with no
# matching source changes.
distro_exempt=" ONEAPI_BUILDER ONEAPI_RUNTIME ROCM_BUILDER ROCM_RUNTIME "
for key in "${image_keys[@]}"; do
  val="${!key:-}"
  case "$distro_exempt" in
    *" $key "*) continue ;;
  esac
  case "$val" in
    *ubuntu24.04* | *ubuntu-24.04*)
      bad "$key is still on Ubuntu 24.04; the tree targets ${DEV_UBUNTU}"
      note "         $key=$val"
      ;;
    *debian12* | *bookworm*)
      bad "$key is still on Debian 12; the release track is Debian ${RELEASE_DEBIAN}"
      note "         $key=$val"
      ;;
  esac
done

# ------------------------------------------------------------ every pin digest --
for key in "${image_keys[@]}"; do
  val="${!key:-}"
  [ -z "$val" ] && continue
  case "$val" in
    *@sha256:*) ;;
    *)
      bad "$key is not digest-pinned: $val"
      note "       A tag alone moves under you; reproducible release builds need the digest."
      ;;
  esac
done

# ------------------------------- rule 4: workflows must not hardcode versions --
# Dockerfiles are only half the problem. The same versions live in CI workflows
# and drifted there too: ROCm was 7.2.4 in build.yml and 7.2.3 in
# libvmaf-build-matrix.yml, and Level Zero existed at four different versions at
# once. Workflow `run:` steps source build-config.env directly; this catches
# anyone who goes back to a literal. Split out because the Level Zero clone puts
# the version and the repository URL on different lines, which a line-oriented
# grep cannot see.
if ! python3 scripts/ci/check-workflow-versions.py; then
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo "check-base-image-single-source: OK (${#files[@]} Dockerfiles, ${#image_keys[@]} pinned bases)"
fi
exit "$fail"
