---
paths:
  - scripts/release/verify-native-release-artifacts.sh
  - scripts/release/tests/test-verify-native-release-artifacts.sh
  - core/include/meson.build
invariant: Both libvmaf + libvmafx chains staged as regular files; DT_RUNPATH=$ORIGIN under env -i.
---
<!-- markdownlint-disable MD013 MD060 -->
# Native release bundle verifier (`verify-native-release-artifacts.sh`)

## verify-native-release-artifacts.sh

Native GitHub Release payload currently Linux ELF. Meson's
`libvmaf.so` -> SONAME -> real-name symlink chain, and since library
split (ADR-2094) same chain of `libvmafx.so`, must be staged under
every name as identical regular-file bytes — GitHub artifact downloads
do not preserve symlinks. CLI and compat `libvmaf.so.3` both NEED
`libvmafx.so.1`: staging one chain alone ships CLI that cannot start.
Verifier checks each chain (`verify_chain`): SONAME, real name, bytes,
CLI's `DT_NEEDED`; rejects missing or divergent chain member.
Requires `ldd` to resolve dependency from staged directory before
running exact CLI version under `env -i`.

RUNPATH contract (T-RELEASE-NATIVE-RUNPATH-BUILD-TREE-2026-09-27): CLI
`DT_RUNPATH` = exactly one entry `$ORIGIN`; zero `DT_RPATH`. Checked
from `readelf --dynamic` before `ldd`; `ldd` and `--version` run under
`env -i PATH=/usr/bin:/bin`, NO `LD_LIBRARY_PATH`. Never re-add
`LD_LIBRARY_PATH` to either run: hid rc.1 build-tree RUNPATH
`$ORIGIN/../src`. Extra entry, trailing `/`, absolute path, legacy
`DT_RPATH` = reject. Value parsed to last `]` of readelf line (inner `]`
never truncates).

Run verifier both before hashing/signing and after artifact
upload/download round trip. Round-trip job restores `vmaf`'s
executable bit first — raw artifact and release downloads do not
carry POSIX mode metadata. Never replace runtime check with
filename-only assertion.

Version check (ADR-1201). `VERSION` argument = narrow tag shape minus
`v`: `X.Y.Z` or `X.Y.Z-rc.N`, no leading zero; anything else exit 64.
`vmaf --version` prints `VMAF_VERSION` from `core/include/meson.build`
`git describe --tags --long --match 'v*.*.*'`, so build checked out
at tag normally reports `vX.Y.Z[-rc.N]-0-g<hex>`, not bare `X.Y.Z`
(reproduced on real CPU builds: `v1.0.0-rc.1-0-gd0f0e7e`,
`v1.0.0-0-g8820048`). Accept exactly two strings: that describe form
(distance `0`, lowercase hex 7–64) or bare `X.Y.Z[-rc.N]` vcs_tag
fallback (tagless checkout; `verify-release-version.sh` pins
`core/meson.build` marker to tag). Never prefix-match: `-N-g` with N>0
= commit after tag, other suffix = not tagged tree (`-dirty` rejected as
defence in depth; `vcs_tag` passes no `--dirty`, so it never appears), `rc.10`
never satisfies `rc.1`. Changing `vcs_tag` command or `--version`
output format -> update verifier + test fixtures in same PR.

Test coverage:
`scripts/release/tests/test-verify-native-release-artifacts.sh`.
