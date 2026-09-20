<!-- markdownlint-disable MD013 -->
# Pelorus v0.2.2 interop parser safety sync — 2026-09-20

- **Status**: Implementation and local verification complete; hosted CI not run
- **VMAFx baseline**: `371ff5891ad43b6d8072d9fac132349ee3ddaaa9`
- **Previous Pelorus pin**: `818d844066e73326c6300c9827ce7324d04cd884`
- **Authoritative release source**:
  `93bef1206d68d9e09024c08a12732fb8e77b9b16` (Pelorus v0.2.2)
- **Related decision**:
  [ADR-1113](../adr/1113-vendor-pelorus-interop-abi.md), 2026-09-20 amendment

## Question

Could VMAFx consume a valid Pelorus side-data blob whose caller-owned byte
buffer is not naturally aligned, and could the mirror remain exact Pelorus
source while still satisfying VMAFx's local format and tidy gates?

## Source delta and ABI result

The authoritative comparison was the pinned Git-object delta, not either
repository's working tree:

```bash
git -C /home/kilian/dev/vmafx/pelorus diff \
  818d844066e73326c6300c9827ce7324d04cd884..93bef1206d68d9e09024c08a12732fb8e77b9b16 \
  -- libpelorus/include/pelorus/pelorus.h \
     libpelorus/src/interop.c libpelorus/test/interop_test.c
```

The release changes the library version from 0.1.0 to 0.2.2, replaces typed
loads/stores at byte-buffer addresses with aligned locals plus `memcpy`, rejects
a `header_size` that is not divisible by eight, and adds two conformance cases.
`PELORUS_ABI_MAJOR` and `PELORUS_ABI_MINOR` remain 1 and 3; no wire field,
section bit, struct size, or section layout changes.

## Sanitizer reproduction: RED before implementation

Only the two released fixture cases were added first. The old VMAFx parser was
then compiled with Clang UBSan:

```bash
CC=clang CXX=clang++ meson setup core/build-pelorus-v022-ubsan core \
  -Db_sanitize=undefined -Db_lto=false -Db_lundef=false \
  -Denable_cuda=false -Denable_sycl=false \
  -Dc_args=-fno-sanitize=function -Dcpp_args=-fno-sanitize=function
meson compile -C core/build-pelorus-v022-ubsan test_pelorus_interop
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  meson test -C core/build-pelorus-v022-ubsan --print-errorlogs \
  test_pelorus_interop
```

The test exited non-zero at `pelorus_interop.c:212`: UBSan reported
`member access within misaligned address` for `const PelorusSideData`, whose
type requires 8-byte alignment. The stack reached the invalid typed access
through `pel_blob_is_present()` when `test_misaligned_blob_base()` re-homed an
otherwise valid blob at a one-byte skew. That demonstrates real C undefined
behavior, not merely a conservative static warning.

## Released fix and GREEN evidence

VMAFx ports v0.2.2's parser exactly. Pack operations construct the header and
directory records as aligned local objects before copying them into the wire
image. Parse operations copy those records out before inspecting fields. The
wire's relative alignment rules remain enforced; the caller's allocation base
does not need to be aligned.

After the port, the same UBSan configure/build/test command passed 1/1 with
`UBSAN_OPTIONS=halt_on_error=1`. A separate normal CPU build also passed 1/1.
The complete fixture now runs sixteen shared vectors, including all base skews
one through seven and the malformed `header_size` rejection.

The fixture proof renders a canonical VMAFx prefix from the exact v0.2.2 Git
object's pin and ABI version, appends Pelorus's body after only the documented
include rewrite, and compares the complete file through EOF; the diff was
empty. Every vendored banner carries the full 40-character source commit.

## Why PR #1351's local lint edits were removed

PR #1351 inserted a VMAFx-only `NOLINTBEGIN`/`NOLINTEND` band and three `(void)`
casts into the shared fixture. Those changes were reasonable local lint answers
but violated ADR-1113's stronger property: both repositories run the same test
body against their respective copies of the parser. A whitespace-insensitive
manual guard allowed that divergence to persist.

The fix moves policy to the correct layer. VMAFx's clang-tidy ratchet now names
only manifest-owned exact mirror paths, including the fixture; the fixture
itself stays unchanged. The drift guard compares every complete rendered file
byte-for-byte through EOF, reads only the pinned Git object, rejects extra
tracked files in lint-exempt namespaces, fails closed if that object is
unavailable, and runs in the existing required Pre-Commit workflow. Regression
fixtures exercise a synthetic re-pin/update, mutate the fixture prefix, add an
unmanifested tracked header, and remove final newlines; every drift case fails
the guard. The synthetic pin rewrite uses portable Python byte IO rather than
GNU-only in-place `sed` syntax.

## Whole-tree tidy verification

The first local CPU-ratchet measurement used the workstation's GCC 16.2.1
headers with clang-tidy 22.1.8. It measured 755 warnings against the normalized
750-warning GCC 15 baseline. All five additions were in files untouched by this
branch:

- `core/src/dict.cpp:125:5`: one
  `cert-dcl03-c,misc-static-assert` diagnostic;
- `core/src/feature/feature_collector.cpp:89:9`, `:239:9`, and `:416:5`:
  three `cert-dcl03-c,misc-static-assert` diagnostics; and
- `core/src/log.c:70:11`: one
  `clang-analyzer-security.VAList,-warnings-as-errors` diagnostic.

That non-canonical result was not treated as completion and did not change the
baseline. The required lane was then reproduced in a disposable Ubuntu 26.04
container using the workflow recipe and exact pinned identities:

```text
gcc-15 (Ubuntu 15.2.0-16ubuntu1) 15.2.0
Ubuntu LLVM version 22.1.8
tidy-ratchet[cpu]: 307 TUs, 750 warnings (baseline 750),
                   0 uncited NOLINTs (baseline 0)
tidy-ratchet: baseline matches measurement
```

The measurement had zero compile failures. The committed 312-unit baseline
normalizes to 306 after removing the six exact-mirror translation units: five
`core/src/interop/pelorus_*.c` files and the shared fixture. The current build
also contains `core/test/test_integer_adm_tiny_frames.c`, which is absent from
that baseline and contributes zero warnings, for 307 selected units and the
same 750-warning total. All Pelorus headers are also outside NOLINT ownership.
Hosted CI remains unrun because this branch is intentionally neither pushed nor
opened as a pull request.

## Maintenance decision

No new architectural alternative was selected: ADR-1113's pinned read-only
mirror remains the design, and ADR-1120's ABI 1.3 decision remains intact. The
dated ADR-1113 amendment records the operational consequence exposed here:
reviewed released parser correctness/security fixes are re-pin triggers even
without an ABI-minor change. A new ADR remains required when the wire ABI or
vendoring architecture changes.

## Residual finding

The exact fixture still calls `fopen(path, "w")` for its x265 CSV test. CodeQL's
world-writable-file finding is not fixed locally because a mirror-only change
would immediately recreate fixture drift. `docs/state.md` tracks it as a
separate open upstream-owned item to fix first in Pelorus and then re-vendor.
