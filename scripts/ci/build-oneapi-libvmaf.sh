#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# scripts/ci/build-oneapi-libvmaf.sh — build libvmaf with oneAPI SYCL,
# stage release artifacts, extract runtime files, and scan licences.
#
# Shared build script called by builder-oneapi2026 in Dockerfile.production-gpu
# and builder-oneapi2026 in Dockerfile.node (HISS-19, ADR-3371).

set -euo pipefail

build_dir="${1:-build}"
repo_root="${2:-$(pwd)}"
dist_dir="${3:-/dist}"
stage_dir="${4:-/stage}"
licence_inputs_dir="${5:-/licence-inputs}"
oneapi_root="${ONEAPI_ROOT:-/opt/intel/oneapi}"
jobs="${VMAF_BUILD_JOBS:-4}"

export ONEAPI_ROOT="${oneapi_root}"
export PATH="${oneapi_root}/compiler/latest/bin:${PATH}"

# The SYCL runtime files libvmaf loads, from this compiler's installation and
# its credist.txt (the Redistributables of the Intel EULA for Developer Tools,
# 2.1.D), go to the image with their licence texts; the compiler stays in the builder.
CC=icx CXX=icpx meson setup "${build_dir}" core \
  --buildtype=release \
  --strip \
  -Db_lto=false \
  -Denable_cuda=false \
  -Denable_sycl=true \
  -Denable_hip=false \
  -Denable_dnn=disabled \
  -Denable_docs=false \
  -Denable_tests=false

nice -n 10 ninja -C "${build_dir}" -j"${jobs}"

mkdir -p "${dist_dir}/lib" "${dist_dir}/bin" "${dist_dir}/model" "${licence_inputs_dir}"
find "${build_dir}/src" -maxdepth 1 \( -name 'libvmaf.so*' -o -name 'libvmafx.so*' \) -not -type d -exec cp -a {} "${dist_dir}/lib/" \;
cp "${build_dir}/tools/vmaf" "${dist_dir}/bin/"
cp -r model/. "${dist_dir}/model/"

python3 tools/rc1-tester/image/prepare_build.py intel-runtime \
  tools/rc1-tester/image/sycl-runtime.json "${oneapi_root}" "${stage_dir}"

python3 tools/rc1-tester/image/licensing.py scan-build \
  --build "${build_dir}" --repo "${repo_root}" \
  --out "${licence_inputs_dir}/vmafx-sources.json"
