#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Build the macOS arm64 tester bundle (ADR-1493). Runs on the hosted macOS arm64
# runner of .github/workflows/macos-tester-bundle.yml, never on a workstation.
#
#   build-macos-tester-bundle.sh <output-dir>
#
# Environment (all required):
#   PBS_URL, PBS_SHA256     python-build-standalone archive and its SHA-256
#   PBS_FULL_URL, PBS_FULL_SHA256
#                           the same release's `full` archive, read only for its
#                           licence texts (python/licenses, PYTHON.json; ADR-1503)
#   VMAF_RESOURCE_COMMIT    Netflix/vmaf_resource commit the fixtures come from
#   VMAFX_SOURCE_COMMIT, VMAFX_SOURCE_REF, VMAFX_RECIPE_COMMIT, VMAFX_IMAGE_TAG
#
# Runs under bash 3.2 (Apple's /bin/bash and the hosted runner's): no mapfile, associative
# arrays, ${x,,}, |&, [[ -v ]], coproc or local -n (tools/rc1-tester/tests/test_bash32_compat.py).
#
# Result in <output-dir>: vmafx-tester-macos-arm64-<tag>.tar.xz, its .sha256,
# report.json (the bundle's own report run on this runner) and bundle-files.txt.
# The bundle carries licenses/ (THIRD_PARTY_NOTICES.txt and every licence text)
# and fails before packing when a file has no recorded licence (ADR-1503).
set -euo pipefail

out=${1:?usage: build-macos-tester-bundle.sh <output-dir>}
for name in PBS_URL PBS_SHA256 PBS_FULL_URL PBS_FULL_SHA256 VMAF_RESOURCE_COMMIT \
  VMAFX_SOURCE_COMMIT VMAFX_SOURCE_REF VMAFX_RECIPE_COMMIT VMAFX_IMAGE_TAG; do
  : "${!name:?$name is required}"
done

repo=$(git rev-parse --show-toplevel)
cd "$repo"
mkdir -p "$out"
out=$(cd "$out" && pwd)
tag=$VMAFX_IMAGE_TAG
name="vmafx-tester-macos-arm64-$tag"
bundle="$out/$name"
build="$repo/build-tester-macos"
image_dir="tools/rc1-tester/image"
resource="$bundle/python/test/resource"
texts="$out/licence-texts"

export MACOSX_DEPLOYMENT_TARGET=14.0
export VMAFX_ARTIFACT_KIND=macos-bundle
export VMAFX_BUILT_BY_WORKFLOW=${VMAFX_BUILT_BY_WORKFLOW:-true}
export VMAFX_BASE_IMAGE="${ImageOS:-macos}-${ImageVersion:-runner}"
export VMAFX_NOT_APPLICABLE='{"golden": "the Python golden stack is not in the macOS bundle; the Netflix pairs are covered by the dispatch and reference checks"}'

step() { printf '\n== %s\n' "$*"; }

step "configure and build (Apple clang, static libvmaf, Metal on, no DNN)"
# zimg: stage static library so Apple clang links it statically with no external dylib references
if ! command -v brew >/dev/null 2>&1; then
  echo "ERROR: brew is required on macOS to locate zimg" >&2
  exit 1
fi
brew_prefix="$(brew --prefix zimg)" || {
  echo "ERROR: brew --prefix zimg failed (zimg not installed via brew)" >&2
  exit 1
}
if [ -z "$brew_prefix" ] || [ ! -f "$brew_prefix/lib/libzimg.a" ]; then
  echo "ERROR: static library libzimg.a not found at $brew_prefix/lib/libzimg.a" >&2
  exit 1
fi
zimg_static_dir="$out/zimg-static"
mkdir -p "$zimg_static_dir/pkgconfig"
sed "s|-L\${libdir} -lzimg|$brew_prefix/lib/libzimg.a|g" "$brew_prefix/lib/pkgconfig/zimg.pc" >"$zimg_static_dir/pkgconfig/zimg.pc"
export PKG_CONFIG_PATH="$zimg_static_dir/pkgconfig:${PKG_CONFIG_PATH:-}"
meson setup "$build" core --buildtype=release --strip --default-library=static -Db_lto=false \
  -Denable_metal=enabled -Denable_dnn=disabled -Denable_cuda=false -Denable_sycl=false \
  -Denable_hip=false -Denable_float=true -Denable_docs=false -Denable_tests=true
# macOS ships bash 3.2: no mapfile. Capture first so a failing select stops the build.
selected=$(python3 "$image_dir/prepare_build.py" select "$build" "$image_dir/unit-tests-macos.txt")
test_targets=()
while IFS= read -r target; do
  [ -n "$target" ] && test_targets+=("$target")
done <<EOF
$selected
EOF
ninja -C "$build" -j4 tools/vmaf "${test_targets[@]}"
if command -v otool >/dev/null 2>&1; then
  if otool -L "$build/tools/vmaf" | grep -qi "zimg"; then
    echo "ERROR: vmaf binary dynamically links zimg dylib instead of static library" >&2
    exit 1
  fi
fi

step "stage the bundle"
mkdir -p "$bundle/build/tools" "$bundle/image" "$bundle/tester" "$resource"
cp "$build/tools/vmaf" "$bundle/build/tools/vmaf"
python3 "$image_dir/prepare_build.py" stage "$build" "$image_dir/unit-tests-macos.txt" "$bundle"
cp -R tools/rc1-tester/src "$bundle/tester/src"
cp tools/rc1-tester/vmaf-tester-report "$bundle/tester/"
cp "$image_dir/fixtures.json" "$bundle/image/fixtures.json"
# ADR-1496: the state rows the report measures, and the parity gate it runs.
cp "$image_dir/metal-rows.json" "$bundle/image/metal-rows.json"
python3 "$image_dir/prepare_build.py" gate "$repo" "$bundle"
cp "$image_dir/macos/run.sh" "$bundle/run.sh"
cp "$image_dir/macos/README.txt" "$bundle/README.txt"
find "$bundle/tester" -name __pycache__ -type d -prune -exec rm -r {} +

step "fixtures (pinned commit, SHA-256 checked)"
python3 "$image_dir/prepare_build.py" fixtures "$image_dir/fixtures.sha256" \
  "$image_dir/fixtures.json" >"$out/fixtures-needed.sha256"
while read -r _ file; do
  curl -fsSL --max-time 600 --retry 3 --create-dirs -o "$resource/$file" \
    "https://raw.githubusercontent.com/Netflix/vmaf_resource/$VMAF_RESOURCE_COMMIT/python/test/resource/$file"
done <"$out/fixtures-needed.sha256"
(cd "$resource" && shasum -a 256 -c "$out/fixtures-needed.sha256")

step "interpreter (python-build-standalone, SHA-256 checked)"
curl -fsSL --max-time 600 --retry 3 -o "$out/pbs.tar.gz" "$PBS_URL"
echo "$PBS_SHA256  $out/pbs.tar.gz" | shasum -a 256 -c -
mkdir "$out/pbs"
tar -xzf "$out/pbs.tar.gz" -C "$out/pbs"
mv "$out/pbs/python" "$bundle/runtime"
# The interpreter archive is an input, not a product: the workflow once published
# every $out/*.tar.gz, and tester-20261003-c12763f3 carried (and signed) it.
rm -r "${out:?}/pbs" "${out:?}/pbs.tar.gz"
# The report needs the standard library only: drop headers, docs, tests, GUI, pip.
for dir in include share; do rm -r "${bundle:?}/runtime/$dir"; done
stdlib=$(echo "$bundle"/runtime/lib/python3.*)
for dir in test idlelib tkinter turtledemo ensurepip lib2to3 site-packages/pip; do
  [ -e "$stdlib/$dir" ] && rm -r "${stdlib:?}/$dir"
done
# Tcl/Tk and the packages that ship with it (itcl, thread, tdbc), and the _tkinter module
# that links them: the report uses none of it and each library would need checking.
find "$bundle/runtime/lib" -maxdepth 1 \( -name 'libtcl*' -o -name 'libtk*' -o -name 'tcl*' \
  -o -name 'tk*' -o -name 'itcl*' -o -name 'thread*' -o -name 'tdbc*' \) -exec rm -r {} +
find "$stdlib/lib-dynload" -name '_tkinter*' -delete
find "$bundle/runtime" -name '*.pyc' -delete
find "$bundle/runtime" -name __pycache__ -type d -prune -exec rm -rf {} +
rm -f "$bundle"/runtime/bin/idle3* "$bundle"/runtime/bin/pydoc3* "$bundle"/runtime/bin/pip*
# The licensing tool needs Python 3.11 or later: run it with the bundled interpreter.
py="$bundle/runtime/bin/python3"
licensing() { "$py" -I -B "$image_dir/licensing.py" "$@"; }

step "licence texts (python-build-standalone full archive, CPython Doc/license.rst)"
# The install_only archive holds no licence texts; the full archive of the same
# release has them for every library linked into the interpreter (ADR-1503).
mkdir -p "$texts/python-build-standalone"
curl -fsSL --max-time 600 --retry 3 -o "$out/pbs-full.tar.zst" "$PBS_FULL_URL"
echo "$PBS_FULL_SHA256  $out/pbs-full.tar.zst" | shasum -a 256 -c -
zstd -dc "$out/pbs-full.tar.zst" | tar -xf - -C "$out" python/licenses python/PYTHON.json
mv "$out"/python/licenses/* "$out/python/PYTHON.json" "$texts/python-build-standalone/"
rm -r "${out:?}/python" "${out:?}/pbs-full.tar.zst"
py_version=$("$py" -I -c 'import platform; print(platform.python_version())')
licensing fetch-texts --artifact macos --python-version "$py_version" --out "$texts"
licensing scan-build --build "$build" --repo "$repo" --out "$out/vmafx-sources.json"

step "ad-hoc code signature (no developer ID, no notarization)"
while IFS= read -r file; do
  codesign --force --sign - "$file"
  codesign --verify --strict "$file"
done < <(find "$bundle/build" "$bundle/tests" -type f -perm -u+x)

step "bundle metadata and references"
python3 "$image_dir/prepare_build.py" info "$bundle"
"$bundle/runtime/bin/python3" -I -B "$bundle/tester/vmaf-tester-report" \
  --image-root "$bundle" generate-reference "$bundle/reference"

step "licence notices (ADR-1503)"
licensing notices --artifact macos --root "$bundle" --repo "$repo" \
  --build-scan "$out/vmafx-sources.json" --texts "$texts" \
  --source-commit "$VMAFX_SOURCE_COMMIT" --tag "$tag"

step "every Mach-O links system libraries or the bundle only"
bash scripts/ci/check-macos-bundle-links.sh "$bundle"

step "the bundle's own report on this runner"
set +e
VMAFX_IMAGE_ROOT="$bundle" "$bundle/run.sh" >"$out/report.json" 2>"$out/report.stderr"
report_status=$?
set -e
cat "$out/report.stderr"
echo "report exit status: $report_status"
case "$report_status" in 0 | 1) ;; *)
  echo "::error::report did not complete ($report_status)"
  exit 1
  ;;
esac

step "every file of the bundle has a recorded licence (ADR-1503)"
licensing check --artifact macos --root "$bundle" --repo "$repo" \
  --build-scan "$out/vmafx-sources.json" --python-version "$py_version" \
  --receipt "$bundle/licence-check.json"

# xz at level 9 (ADR-1591): macOS's own tar and Archive Utility read xz through the
# liblzma Apple builds libarchive with (zstd is not built in), and level 9's 64 MiB
# window finds the frame-to-frame redundancy of the test videos that gzip's 32 KiB
# window cannot. libarchive's xz writer runs one thread, so the archive is reproducible.
step "pack"
(cd "$bundle" && find . -type f | sort | xargs -I{} stat -f '%z %N' {}) >"$out/bundle-files.txt"
(cd "$out" && COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --options xz:compression-level=9 \
  -cJf "$name.tar.xz" "$name")
(cd "$out" && shasum -a 256 "$name.tar.xz" >"$name.tar.xz.sha256")
cat "$out/$name.tar.xz.sha256"
ls -l "$out/$name.tar.xz"
