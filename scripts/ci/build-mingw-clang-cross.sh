#!/usr/bin/env bash
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
# Build VMAFx for Windows with clang/lld targeting x86_64-w64-windows-gnu.
#
# The compiler driver is expected to be the llvm-mingw cross wrapper
# (x86_64-w64-mingw32-clang), with x86_64-w64-mingw32-{ar,ranlib,strip,windres}
# beside it on PATH. The build is deliberately CPU-only and shared-only:
# ADR-3397's LTO import-library fix is gated on cc.get_id() == 'gcc', so a
# clang/lld DLL must be built with -Db_lto=false until clang is measured.
set -euo pipefail

build_dir="${1:-build-mingw-clang}"
prefix="${2:-$PWD/install-mingw-clang}"
stage_dir="${STAGE_DIR:-$PWD/artifact-mingw-clang}"

command -v x86_64-w64-mingw32-clang >/dev/null || {
    echo "x86_64-w64-mingw32-clang not found; put llvm-mingw/bin on PATH" >&2
    exit 1
}

meson setup "$build_dir" core \
    --cross-file build-aux/x86_64-w64-mingw32-clang.ini \
    --buildtype release \
    --default-library shared \
    --prefix "$prefix" \
    -Db_lto=false \
    -Denable_tests=false \
    -Denable_docs=false \
    -Denable_zimg=false \
    -Denable_cuda=false \
    -Denable_sycl=false \
    -Denable_dnn=disabled \
    -Denable_asm=false

meson install -C "$build_dir"

# ADR-3397: a shared MinGW DLL must expose T <fn> thunks in its import library.
# llvm-mingw has no libgcc_eh/libgcc/libwinpthread archives in the GCC layout,
# so only the thunk half of the checker applies here.
NM="${NM:-x86_64-w64-mingw32-nm}" \
python3 scripts/ci/check_mingw_import_thunks.py --dll-dir "$build_dir/src" "$build_dir"

staged="${stage_dir}"
rm -rf "$staged"
mkdir -p "$staged"
cp "$prefix"/bin/*.exe "$prefix"/bin/*.dll "$staged"/

# llvm-mingw carries its C++ runtime as DLLs, not as import libraries.
# Stage them next to the build products so the executable can be run on a
# Windows host without a toolchain install.
toolchain_root="${LLVM_MINGW_ROOT:-$(cd "$(dirname "$(command -v x86_64-w64-mingw32-clang)")/.." && pwd)}"
runtime_dir="$toolchain_root/x86_64-w64-mingw32/bin"
for dll in libc++.dll libunwind.dll libwinpthread-1.dll; do
    if [ -f "$runtime_dir/$dll" ]; then
        cp "$runtime_dir/$dll" "$staged/"
    fi
done

echo "staged $staged"
