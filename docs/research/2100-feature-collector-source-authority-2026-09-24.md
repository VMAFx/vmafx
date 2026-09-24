# Research-2100: feature-collector source authority — 2026-09-24

## Finding

`core/src/feature/feature_collector.c` was a real resurrection, not a harmless
test fixture. Commit `e22a0b2b8` renamed the implementation to
`feature_collector.cpp` and made the production feature library consume it.
Commit `5d070b0b4` later added a new `feature_collector.c` while retaining the
C++ file, and changed production Meson back to the C source. Both commits are
ancestors of exact `origin/master` `4e6916d16ac57647105d14a47a6680117d6b5738`.

The configured CPU build made the split observable:

| Consumer | Translation unit compiled |
| --- | --- |
| `src/liblibvmaf_feature.a` (production) | `feature_collector.c` |
| `test_predict` | `feature_collector.cpp` |
| `test_feature_collector_coverage` | `feature_collector.cpp` |
| `test_feature_collector` | text-included `feature_collector.c` |

`ar` and `nm` confirmed that the shipped feature archive contained only the C
object, while the two tests exercised a separately compiled C++ object. The
ADR-1135 twin-drift gate reported the C++ side as test-only and passed because
both files appeared in some build target; it cannot establish that two
compiled implementations are semantically equivalent.

## Divergence and selected repair

The production C implementation had accumulated fixes that the test-only C++
copy did not have: mutex coverage for model mount/unmount and metadata
registration, the complete model-pointer snapshot that closes the iter10 TSan
race, the unlocked destroy helper that avoids self-deadlock, the HISS-01
no-`goto` unwind helpers, current capacity constants, and the `-EAGAIN` score
read contract. Switching Meson to the stale C++ file would therefore have
reintroduced live concurrency and lifetime defects.

The repair promotes the current production body to the sole C++ translation
unit, retains its C linkage and every hardening change, restores the internal
helper declarations used by the C tests, removes the C source, and points the
production build at `feature_collector.cpp`. The existing white-box tests now
compile or link the same authoritative source instead of another
implementation. `test_feature_collector_source_authority.py` is the red-cap:
it requires the C++ file, rejects a sibling C implementation, checks the
production source-list entry, rejects any build-file reference to the C twin,
and rejects textual inclusion of either implementation source. On the pre-fix
tree all four predicates failed; after the repair all four pass.

## Alternatives considered

| Option | Result |
| --- | --- |
| Delete `.cpp` and keep production `.c` | Rejected: it abandons the already-landed C++ migration and leaves the explicit build gap unresolved. |
| Point production Meson at the existing `.cpp` | Rejected: that copy lacked the mutex, TSan, unwind, and destroy fixes listed above. |
| Keep both sources and strengthen the generic twin gate | Rejected: duplicate implementations can still drift semantically while both remain compiled. |
| Port the current production body into one `.cpp` and delete `.c` | Selected: one implementation serves production and tests without changing the C ABI or score behavior. |

No ADR is needed: this is the bug-fix completion of the existing C++ migration
and closes an already-recorded gap; it introduces no new public surface or
architecture decision. No FFmpeg patch is needed because public headers and
symbols are unchanged.

## Reproducer and verification

Configure the CPU profile and inspect the collector entries in the compile
database:

```bash
meson setup build-fc core --buildtype=release \
  -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
  -Denable_metal=disabled -Denable_dnn=disabled
ninja -C build-fc src/liblibvmaf_feature.a \
  test/test_feature_collector test/test_predict \
  test/test_feature_collector_coverage test/test_flush_context_ordering
meson test -C build-fc test_feature_collector \
  test_feature_collector_source_authority test_predict \
  test_feature_collector_coverage test_flush_context_ordering --print-errorlogs
```

Verification on the repaired tree produced these receipts:

- full fast suite: 146/146 passed in `build-fc-verify`;
- focused CPU rerun: 5/5 collector, predict, coverage, flush-ordering, and
  source-authority tests passed;
- ASan + UBSan focused rerun: the same 5/5 passed without sanitizer findings;
- exact CPU tidy lane toolchain (`gcc-15` 15.2.0 and apt.llvm.org
  `clang-tidy-22` 22.1.8): the collector TU fell from 13 warnings to zero and
  the generated scoped baseline tightened accordingly; and
- twin-drift, state-row, changelog/ADR-fragment, Ruff, clang-format, and
  diff-whitespace gates passed.
