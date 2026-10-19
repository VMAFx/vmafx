#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Hardware smoke test for vmaf_vpl on Intel GPU with VA-API + VPL zero-copy.
# Runs only when Intel GPU with VA-API is available and idle; skips (77) otherwise.
# Tagged in meson with suite : ['slow', 'gpu'] and is_parallel : false.

set -eu

BIN="./tools/vmaf_vpl"
if [ ! -x "${BIN}" ]; then
  echo "test_vmaf_vpl_hardware_smoke: ${BIN} not built, skipping" >&2
  exit 77
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "test_vmaf_vpl_hardware_smoke: ffmpeg not found, skipping" >&2
  exit 77
fi

ROOT="${MESON_SOURCE_ROOT:-${PWD}/..}"
REF_SRC="${ROOT}/testdata/ref_576x324_48f.yuv"
DIS_SRC="${ROOT}/testdata/dis_576x324_48f.yuv"

if [ ! -f "${REF_SRC}" ] || [ ! -f "${DIS_SRC}" ]; then
  REF_SRC="${ROOT}/../testdata/ref_576x324_48f.yuv"
  DIS_SRC="${ROOT}/../testdata/dis_576x324_48f.yuv"
fi

if [ ! -f "${REF_SRC}" ] || [ ! -f "${DIS_SRC}" ]; then
  echo "test_vmaf_vpl_hardware_smoke: test fixtures not found, skipping" >&2
  exit 77
fi

# Locate an Intel DRM render node with working iHD VA-API
INTEL_NODE=""
if command -v vainfo >/dev/null 2>&1; then
  for node in /dev/dri/renderD*; do
    if [ -e "$node" ] && [ -r "$node" ] && [ -w "$node" ]; then
      if LIBVA_DRIVER_NAME=iHD vainfo --display drm --device "$node" 2>&1 | grep -iq "Intel iHD driver"; then
        INTEL_NODE="$node"
        break
      fi
    fi
  done
fi

if [ -z "$INTEL_NODE" ]; then
  echo "test_vmaf_vpl_hardware_smoke: no Intel VA-API device found, skipping" >&2
  exit 77
fi

WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_vpl_smoke.scratch"
mkdir -p "${WORK}"

REF_H264="${WORK}/smoke_ref.h264"
DIS_H264="${WORK}/smoke_dis.h264"

# Generate short 12-frame H.264 elementary streams
ffmpeg -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 -i "${REF_SRC}" -frames:v 12 -c:v libx264 -g 12 -bf 2 "${REF_H264}" >/dev/null 2>&1
ffmpeg -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 -i "${DIS_SRC}" -frames:v 12 -c:v libx264 -g 12 -bf 2 "${DIS_H264}" >/dev/null 2>&1

SMOKE_LOG="${WORK}/smoke.stdout"
SMOKE_ERR="${WORK}/smoke.stderr"

# 1. Zero-copy hardware decode smoke
if ! LIBVA_DRIVER_NAME=iHD "${BIN}" \
  --ref "${REF_H264}" \
  --dis "${DIS_H264}" \
  --render-node "${INTEL_NODE}" \
  --frames 12 \
  >"${SMOKE_LOG}" 2>"${SMOKE_ERR}"; then
  echo "test_vmaf_vpl_hardware_smoke: vmaf_vpl failed on ${INTEL_NODE}" >&2
  cat "${SMOKE_ERR}" >&2
  exit 1
fi

if grep -q "DecodeFrameAsync yielded no frame" "${SMOKE_ERR}"; then
  echo "test_vmaf_vpl_hardware_smoke: unexpected decode ceiling exhaustion on healthy run" >&2
  cat "${SMOKE_ERR}" >&2
  exit 1
fi

if grep -q "Decode error" "${SMOKE_ERR}"; then
  echo "test_vmaf_vpl_hardware_smoke: unexpected decode error on healthy run" >&2
  cat "${SMOKE_ERR}" >&2
  exit 1
fi

# 2. Host upload fallback path
FB_LOG="${WORK}/smoke_fb.stdout"
FB_ERR="${WORK}/smoke_fb.stderr"

if ! LIBVA_DRIVER_NAME=iHD "${BIN}" \
  --ref "${REF_H264}" \
  --dis "${DIS_H264}" \
  --render-node "${INTEL_NODE}" \
  --frames 12 \
  --fallback \
  >"${FB_LOG}" 2>"${FB_ERR}"; then
  echo "test_vmaf_vpl_hardware_smoke: vmaf_vpl --fallback failed on ${INTEL_NODE}" >&2
  cat "${FB_ERR}" >&2
  exit 1
fi

if grep -q "DecodeFrameAsync yielded no frame" "${FB_ERR}"; then
  echo "test_vmaf_vpl_hardware_smoke: unexpected decode ceiling exhaustion on fallback run" >&2
  cat "${FB_ERR}" >&2
  exit 1
fi

rm -rf "${WORK}"
echo "test_vmaf_vpl_hardware_smoke: PASS on ${INTEL_NODE}"
exit 0
