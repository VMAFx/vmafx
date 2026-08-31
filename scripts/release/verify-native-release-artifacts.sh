#!/usr/bin/env bash
# Verify that staged Linux release artifacts form a runnable ELF bundle.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

usage() {
  printf 'Usage: verify-native-release-artifacts.sh ARTIFACT_DIR VERSION\n'
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

if [[ $# -ne 2 ]]; then
  usage >&2
  exit 64
fi

artifact_dir="$1"
expected_version="$2"

if [[ ! "$expected_version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  printf 'ERROR: expected version must be ordinary SemVer MAJOR.MINOR.PATCH: %s\n' \
    "$expected_version" >&2
  exit 64
fi
[[ -d "$artifact_dir" ]] || die "artifact directory does not exist: $artifact_dir"

for tool in ldd readelf realpath sha256sum; do
  command -v "$tool" >/dev/null || die "required tool is unavailable: $tool"
done

artifact_dir="$(realpath -- "$artifact_dir")"
cli="$artifact_dir/vmaf"
unversioned_library="$artifact_dir/libvmaf.so"

[[ -f "$cli" && ! -L "$cli" ]] || die "vmaf must be a regular staged file"
[[ -x "$cli" ]] || die "vmaf is not executable: $cli"
[[ -s "$unversioned_library" && ! -L "$unversioned_library" ]] ||
  die "libvmaf.so must be a non-empty regular staged file"

mapfile -t sonames < <(
  LC_ALL=C readelf --dynamic -- "$unversioned_library" |
    sed -n 's/.*(SONAME).*\[\([^]]*\)\].*/\1/p'
)
if [[ ${#sonames[@]} -ne 1 ]]; then
  die "libvmaf.so must declare exactly one ELF SONAME"
fi
soname="${sonames[0]}"
if [[ ! "$soname" =~ ^libvmaf\.so\.(0|[1-9][0-9]*)$ ]]; then
  die "unexpected libvmaf SONAME: $soname"
fi
soname_library="$artifact_dir/$soname"
[[ -s "$soname_library" && ! -L "$soname_library" ]] ||
  die "staged SONAME file is missing, empty, or a symlink: $soname"

shopt -s nullglob
realname_candidates=("$artifact_dir"/libvmaf.so.*.*.*)
shopt -u nullglob
realname_libraries=()
for candidate in "${realname_candidates[@]}"; do
  candidate_name="$(basename -- "$candidate")"
  if [[ "$candidate_name" =~ ^libvmaf\.so\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    realname_libraries+=("$candidate")
  fi
done
if [[ ${#realname_libraries[@]} -ne 1 ]]; then
  die "expected exactly one staged libvmaf.so.ABI_MAJOR.ABI_MINOR.ABI_PATCH real name"
fi
realname_library="${realname_libraries[0]}"
[[ -s "$realname_library" && ! -L "$realname_library" ]] ||
  die "staged libvmaf real-name file is empty or a symlink"

reference_hash="$(sha256sum -- "$unversioned_library" | cut -d' ' -f1)"
for library in "$soname_library" "$realname_library"; do
  library_hash="$(sha256sum -- "$library" | cut -d' ' -f1)"
  if [[ "$library_hash" != "$reference_hash" ]]; then
    die "staged libvmaf link-chain names do not contain identical bytes"
  fi
done

mapfile -t needed_libraries < <(
  LC_ALL=C readelf --dynamic -- "$cli" |
    sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p'
)
soname_needed=false
for needed_library in "${needed_libraries[@]}"; do
  if [[ "$needed_library" == "$soname" ]]; then
    soname_needed=true
    break
  fi
done
[[ "$soname_needed" == true ]] || die "vmaf does not declare $soname as a dependency"

if ! ldd_output="$(
  env -i PATH=/usr/bin:/bin LD_LIBRARY_PATH="$artifact_dir" \
    /usr/bin/ldd "$cli" 2>&1
)"; then
  printf '%s\n' "$ldd_output" >&2
  die "vmaf dependency resolution failed in the clean environment"
fi
mapfile -t resolved_libraries < <(
  printf '%s\n' "$ldd_output" |
    awk -v soname="$soname" '$1 == soname && $2 == "=>" { print $3 }'
)
if [[ ${#resolved_libraries[@]} -ne 1 ]]; then
  printf '%s\n' "$ldd_output" >&2
  die "clean dependency resolution did not resolve exactly one $soname"
fi
resolved_library="$(realpath -- "${resolved_libraries[0]}")"
if [[ "$resolved_library" != "$soname_library" ]]; then
  die "$soname resolved outside the staged artifact directory: $resolved_library"
fi

if ! version_output="$(
  env -i PATH=/usr/bin:/bin LD_LIBRARY_PATH="$artifact_dir" \
    "$cli" --version 2>&1
)"; then
  printf '%s\n' "$version_output" >&2
  die "staged vmaf failed to run in the clean environment"
fi
if [[ "$version_output" != "$expected_version" ]]; then
  die "staged vmaf reported '$version_output', expected '$expected_version'"
fi

printf 'Verified Linux release runtime %s with materialized %s chain.\n' \
  "$expected_version" "$soname"
