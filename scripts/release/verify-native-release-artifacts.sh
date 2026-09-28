#!/usr/bin/env bash
# Verify that staged Linux release artifacts form a runnable ELF bundle.
#
# The bundle is one flat directory: the vmaf CLI next to every libvmaf chain
# name. The CLI must find libvmaf through its own RUNPATH, exactly `$ORIGIN`,
# so the dependency check and the version run below set no LD_LIBRARY_PATH.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail
# Bracket ranges such as [0-9] match non-ASCII digits in some UTF-8 locales.
export LC_ALL=C

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

# ADR-1201: the same narrow shape scripts/release/verify-release-version.sh
# accepts for the tag, minus its leading `v` -- a plain triple, optionally
# followed by `-rc.N` with no leading zero. Nothing else (`-beta`, `-rc`,
# `-rc.01`, `-rc.1.2`) is a version this fork releases.
if [[ ! "$expected_version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-rc\.(0|[1-9][0-9]*))?$ ]]; then
  printf 'ERROR: expected version must be MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-rc.N: %s\n' \
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
container_provenance="$artifact_dir/container-build-provenance.txt"

[[ -f "$cli" && ! -L "$cli" ]] || die "vmaf must be a regular staged file"
[[ -x "$cli" ]] || die "vmaf is not executable: $cli"
[[ -s "$unversioned_library" && ! -L "$unversioned_library" ]] ||
  die "libvmaf.so must be a non-empty regular staged file"
[[ -s "$container_provenance" && ! -L "$container_provenance" ]] ||
  die "staged container-build provenance is missing, empty, or a symlink"

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

if ! cli_dynamic="$(readelf --dynamic -- "$cli")"; then
  die "cannot read the ELF dynamic section of vmaf"
fi
mapfile -t needed_libraries < <(
  printf '%s\n' "$cli_dynamic" |
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

# The loader searches DT_RUNPATH for the CLI's direct dependencies, so the
# bundle needs exactly one entry, `$ORIGIN`, and no DT_RPATH (which the loader
# ignores beside a RUNPATH and which can only carry a stale path). Meson's
# build-tree RUNPATH `$ORIGIN/../src`, shipped in the v1.0.0-rc.1 asset, finds
# nothing next to the downloaded CLI; an extra entry would let a directory
# outside the bundle supply libvmaf (T-RELEASE-NATIVE-RUNPATH-BUILD-TREE-2026-09-27).
# The value runs to the last `]` of readelf's line, so a `]` inside it cannot
# cut it short.
# shellcheck disable=SC2016 # $ORIGIN is for the dynamic loader, not the shell.
expected_runpath='$ORIGIN'
mapfile -t cli_runpaths < <(
  printf '%s\n' "$cli_dynamic" | sed -n 's/.*(RUNPATH)[^[]*\[\(.*\)\]$/\1/p'
)
mapfile -t cli_rpaths < <(
  printf '%s\n' "$cli_dynamic" | sed -n 's/.*(RPATH)[^[]*\[\(.*\)\]$/\1/p'
)
if [[ ${#cli_rpaths[@]} -ne 0 ]]; then
  die "vmaf must carry no DT_RPATH, found: ${cli_rpaths[*]}"
fi
if [[ ${#cli_runpaths[@]} -ne 1 || "${cli_runpaths[0]}" != "$expected_runpath" ]]; then
  die "vmaf RUNPATH must be exactly $expected_runpath, found: ${cli_runpaths[*]:-(none)}"
fi

if ! ldd_output="$(
  env -i PATH=/usr/bin:/bin /usr/bin/ldd "$cli" 2>&1
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

if ! version_output="$(env -i PATH=/usr/bin:/bin "$cli" --version 2>&1)"; then
  printf '%s\n' "$version_output" >&2
  die "staged vmaf failed to run in the clean environment"
fi
# `vmaf --version` prints VMAF_VERSION, which core/include/meson.build takes
# from `git describe --tags --long --match 'v*.*.*'` and, only when that
# command fails, from the vcs_tag fallback meson.project_version(). A release
# build checks out the tag itself, so describe succeeds and reports
# `v<version>-0-g<object name>` (reproduced: tag v1.0.0-rc.1 ->
# "v1.0.0-rc.1-0-gd0f0e7e", tag v1.0.0 -> "v1.0.0-0-g8820048"). Accept exactly
# that string, or exactly the bare fallback, which verify-release-version.sh
# pins to the tag through the coordinated core/meson.build marker. Nothing
# looser: a non-zero distance is a build of a commit after the tag, and a
# `-dirty` or any other suffix is not the tagged tree.
describe_prefix="v${expected_version}-0-g"
if [[ "$version_output" == "$expected_version" ]]; then
  reported_form='project version (no tag reachable at build time)'
elif [[ "$version_output" == "$describe_prefix"* &&
  "${version_output#"$describe_prefix"}" =~ ^[0-9a-f]{7,64}$ ]]; then
  reported_form='git describe exactly on the release tag'
else
  die "staged vmaf reported '$version_output', expected '$expected_version' or '${describe_prefix}<commit>'"
fi

printf 'Verified Linux release runtime %s (%s: %s) with materialized %s chain and RUNPATH %s.\n' \
  "$expected_version" "$reported_form" "$version_output" "$soname" "$expected_runpath"
