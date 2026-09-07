#!/usr/bin/env bash
# Install a pruned ROCm tree onto a CI runner from a digest-pinned OCI image.
#
# Why this exists (ADR-1225): since ROCm 7.14 AMD builds and releases through
# "TheRock", and the legacy apt channel at repo.radeon.com/rocm/apt/ tops out
# at 7.2.4 (its own `latest` resolves there; apt/7.14 and apt/10.0.0 both
# 404). The manylinux channel stops at rocm-rel-7.2.4 and the TheRock wheel
# index carries only 7.14.0 alphas, so for ROCm >= 7.14 the official container
# image is the only stable, digest-pinnable artifact.
#
# `docker pull` is not an option on a GitHub-hosted runner: the 10.0.0-full
# image is 8.2 GB compressed / 29 GB extracted, and the HIP leg shares its
# runner with the CUDA toolkit and oneAPI. This script instead streams each
# layer blob straight from the registry into tar, extracting only /opt/rocm
# and skipping the math libraries libvmaf never links (hipBLASLt, rocBLAS,
# MIOpen, Composable Kernel's device-op archives, rocFFT/rocSPARSE/rocSOLVER,
# the profilers). Nothing is ever stored whole: peak disk is the ~5.5 GB
# result, not the 29 GB image.
#
# Usage:
#   scripts/ci/install-rocm-from-image.sh \
#       --image rocm/dev-ubuntu-24.04@sha256:<digest> \
#       --dest /opt/rocm
#
# Requires: curl, jq, tar, gzip; zstd only if the registry serves zstd layers.
set -euo pipefail

IMAGE=""
DEST="/opt/rocm"
REGISTRY="registry-1.docker.io"
AUTH="https://auth.docker.io/token"
AUTH_SERVICE="registry.docker.io"

while [ $# -gt 0 ]; do
  case "$1" in
    --image)
      IMAGE="$2"
      shift 2
      ;;
    --dest)
      DEST="$2"
      shift 2
      ;;
    *)
      echo "install-rocm-from-image.sh: unknown argument '$1'" >&2
      exit 2
      ;;
  esac
done

if [ -z "$IMAGE" ]; then
  echo "install-rocm-from-image.sh: --image <repo@sha256:...> is required" >&2
  exit 2
fi

case "$IMAGE" in
  *@sha256:*) ;;
  *)
    # A tag is reproducible only for as long as nobody moves it. The whole
    # point of pinning ROCm is that the toolchain does not change under a
    # green CI run, so refuse anything but a digest.
    echo "install-rocm-from-image.sh: --image must be digest-pinned (repo@sha256:...)" >&2
    exit 2
    ;;
esac

REPO="${IMAGE%@*}"
DIGEST="${IMAGE#*@}"

# Paths under /opt/rocm that libvmaf's HIP backend never links. Kept as tar
# --exclude patterns so they are skipped during extraction rather than
# written and then deleted.
EXCLUDES=(
  'opt/rocm/*/lib/hipblaslt'
  'opt/rocm/*/lib/hipsparselt'
  'opt/rocm/*/lib/rocblas'
  'opt/rocm/*/lib/rdc'
  'opt/rocm/*/lib/rocprofiler-systems'
  'opt/rocm/*/lib/rocprofiler-compute'
  'opt/rocm/*/lib/libdevice_*_operations.a'
  'opt/rocm/*/lib/librocshmem.a'
  'opt/rocm/*/lib/librocroller.so*'
  'opt/rocm/*/lib/librocjitsu.so*'
  'opt/rocm/*/lib/libMIOpen*'
  'opt/rocm/*/lib/librocsparse*'
  'opt/rocm/*/lib/librocsolver*'
  'opt/rocm/*/lib/librocfft*'
  'opt/rocm/*/lib/librocrand*'
  'opt/rocm/*/lib/librocblas*'
  'opt/rocm/*/lib/libhipblas*'
  'opt/rocm/*/lib/libhipfft*'
  'opt/rocm/*/lib/libhipsparse*'
  'opt/rocm/*/lib/libhipsolver*'
  'opt/rocm/*/lib/libhiprand*'
  'opt/rocm/*/lib/librccl*'
  'opt/rocm/*/lib/librocprof-sys*'
  'opt/rocm/*/lib/librocprofiler-sdk*'
  'opt/rocm/*/share/miopen'
  'opt/rocm/*/share/doc'
)
# NOTE: librocprofiler-register.so.0 is deliberately NOT excluded --
# libamdhip64.so links it, and dropping it makes every HIP binary fail at
# load with "cannot open shared object file".

tar_excludes=()
for pattern in "${EXCLUDES[@]}"; do
  tar_excludes+=("--exclude=$pattern")
done

echo "==> Authenticating to $REGISTRY for $REPO"
TOKEN="$(curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors "${AUTH}?service=${AUTH_SERVICE}&scope=repository:${REPO}:pull" | jq -r .token)"
if [ -z "$TOKEN" ] || [ "$TOKEN" = "null" ]; then
  echo "install-rocm-from-image.sh: failed to obtain a registry pull token" >&2
  exit 1
fi

ACCEPT='application/vnd.docker.distribution.manifest.v2+json,application/vnd.docker.distribution.manifest.list.v2+json,application/vnd.oci.image.manifest.v1+json,application/vnd.oci.image.index.v1+json'

fetch_manifest() {
  curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors \
    -H "Authorization: Bearer ${TOKEN}" -H "Accept: ${ACCEPT}" \
    "https://${REGISTRY}/v2/${REPO}/manifests/$1"
}

echo "==> Fetching manifest $DIGEST"
MANIFEST="$(fetch_manifest "$DIGEST")"

# A multi-arch index has `manifests[]`; resolve it to the linux/amd64 child.
if [ "$(printf '%s' "$MANIFEST" | jq -r 'has("manifests")')" = "true" ]; then
  CHILD="$(printf '%s' "$MANIFEST" |
    jq -r '.manifests[] | select(.platform.os=="linux" and .platform.architecture=="amd64") | .digest' |
    head -n1)"
  if [ -z "$CHILD" ] || [ "$CHILD" = "null" ]; then
    echo "install-rocm-from-image.sh: no linux/amd64 manifest in the index" >&2
    exit 1
  fi
  echo "==> Index resolved to linux/amd64 manifest $CHILD"
  MANIFEST="$(fetch_manifest "$CHILD")"
fi

LAYER_COUNT="$(printf '%s' "$MANIFEST" | jq -r '.layers | length')"
if [ -z "$LAYER_COUNT" ] || [ "$LAYER_COUNT" = "null" ] || [ "$LAYER_COUNT" -eq 0 ]; then
  echo "install-rocm-from-image.sh: manifest carries no layers" >&2
  exit 1
fi
echo "==> $LAYER_COUNT layers to scan"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# Layers apply in order, so extracting them in order reproduces the image's
# view of /opt/rocm (later layers overwrite earlier ones). Whiteout entries
# (.wh.*) are not honoured: ROCm is installed by a single layer that never
# deletes a path a later layer needs, and a stray .wh. file under /opt/rocm
# would be inert anyway.
i=0
while [ "$i" -lt "$LAYER_COUNT" ]; do
  layer_digest="$(printf '%s' "$MANIFEST" | jq -r ".layers[$i].digest")"
  layer_media="$(printf '%s' "$MANIFEST" | jq -r ".layers[$i].mediaType")"
  case "$layer_media" in
    *zstd*) decomp=(zstd -dc) ;;
    *gzip*) decomp=(gzip -dc) ;;
    *+tar | *.tar) decomp=(cat) ;;
    *) decomp=(gzip -dc) ;;
  esac
  echo "==> layer $((i + 1))/$LAYER_COUNT ($layer_media)"
  # `|| true` on tar: a layer with no opt/rocm/ member makes GNU tar exit 2
  # with "Not found in archive", which is the normal case for most layers.
  curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors \
    -H "Authorization: Bearer ${TOKEN}" \
    "https://${REGISTRY}/v2/${REPO}/blobs/${layer_digest}" |
    "${decomp[@]}" |
    tar -x -C "$STAGE" "${tar_excludes[@]}" --wildcards 'opt/rocm/*' 2>/dev/null || true
  i=$((i + 1))
done

if [ ! -d "$STAGE/opt/rocm" ]; then
  echo "install-rocm-from-image.sh: no /opt/rocm found in $IMAGE" >&2
  exit 1
fi

# Overlay whiteout markers are an artefact of layering, not content.
find "$STAGE/opt/rocm" -name '.wh.*' -delete 2>/dev/null || true

# ROCm 10 installs the real tree under /opt/rocm/core-<major>.<minor>/ and
# makes /opt/rocm/{bin,lib,include,llvm,share,libexec,amdgcn} symlinks into
# /etc/alternatives/rocm-*, which point back into that tree. Extracting only
# /opt/rocm therefore yields a directory of dangling symlinks. Rather than
# writing into the runner's /etc/alternatives -- which is shared with every
# other package on the machine -- repoint each one at the core directory
# relative to /opt/rocm, so the extracted tree is self-contained.
core_dir=""
for candidate in "$STAGE"/opt/rocm/core-*; do
  if [ -d "$candidate" ]; then
    core_dir="$(basename "$candidate")"
    break
  fi
done

if [ -n "$core_dir" ]; then
  echo "==> Repointing /etc/alternatives symlinks at $core_dir"
  for entry in "$STAGE"/opt/rocm/*; do
    [ -L "$entry" ] || continue
    target="$(readlink "$entry")"
    case "$target" in
      /etc/alternatives/*) ;;
      *) continue ;;
    esac
    name="$(basename "$entry")"
    if [ -e "$STAGE/opt/rocm/$core_dir/$name" ]; then
      ln -sfn "$core_dir/$name" "$entry"
    elif [ "${name#core}" != "$name" ]; then
      # /opt/rocm/core and /opt/rocm/core-<major> both alias the core dir.
      ln -sfn "$core_dir" "$entry"
    else
      echo "install-rocm-from-image.sh: dangling /opt/rocm/$name -> $target" >&2
      exit 1
    fi
  done
fi

echo "==> Extracted $(du -sh "$STAGE/opt/rocm" | cut -f1) of ROCm"

SUDO=""
if [ "$(id -u)" -ne 0 ] && ! mkdir -p "$(dirname "$DEST")" 2>/dev/null; then
  SUDO="sudo"
fi
$SUDO rm -rf "$DEST"
$SUDO mkdir -p "$(dirname "$DEST")"
$SUDO mv "$STAGE/opt/rocm" "$DEST"

# hipcc is a thin driver over the bundled LLVM; if either half is missing the
# failure otherwise surfaces much later as a confusing meson probe error.
if [ ! -x "$DEST/bin/hipcc" ]; then
  echo "install-rocm-from-image.sh: $DEST/bin/hipcc missing after extraction" >&2
  exit 1
fi
if ! ls "$DEST"/lib/libamdhip64.so* >/dev/null 2>&1; then
  echo "install-rocm-from-image.sh: $DEST/lib/libamdhip64.so missing after extraction" >&2
  exit 1
fi

echo "==> ROCm installed at $DEST"
"$DEST/bin/hipconfig" --version || true
