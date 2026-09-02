# Research digest: clang-tidy `modernize-*` sweep (2026-05-31)

## Goal

Audit the `modernize-*` family of clang-tidy checks for adoption on the fork,
quantify the impact on fork-added C++ translation units, and discharge the
top findings in the same PR that flips the configuration.

## Method

1. Built the CPU-only meson tree with `compile_commands.json` enabled:
   `meson setup build-modernize core -Denable_cuda=false -Denable_sycl=false`,
   then `ninja -C build-modernize`.
2. Ran `clang-tidy -p build-modernize --checks='-*,modernize-*' --header-filter='^core/(src|tools|test)/'`
   against every fork-added CPU-built `.cpp` translation unit
   (`core/src/cpu.cpp`, `core/src/feature/feature_collector.cpp`,
   `core/src/metadata_handler.cpp`).
3. Counted findings per check (raw output, no NOLINT filtering).
4. Triaged each check into one of three buckets: high-value (fix in this PR),
   noisy-but-correct (defer with explicit opt-out), false-positive on our C ABI
   (permanent opt-out).

## Findings (pre-fix)

| Check | Count | Decision |
| --- | --- | --- |
| `modernize-use-trailing-return-type` | 21 | Defer (cosmetic; needs fork-wide style ADR) |
| `modernize-use-nullptr` | 8 | Fix (already partially enabled in baseline) |
| `modernize-deprecated-headers` | 6 | Fix (C → C++23 migration ratchet) |
| `modernize-use-auto` | 2 | Fix the 1 high-confidence (cast initializer); skip the other |
| `modernize-use-override` | 0 | Already enabled in baseline; no findings |

Total fixed in this PR: 15 (matches the requested top-15 target — 8 nullptr +
6 deprecated-headers + 1 use-auto).

## Findings (post-fix, same files)

| Check | Count |
| --- | --- |
| `modernize-use-trailing-return-type` | 21 (intentionally deferred) |
| `modernize-use-auto` | 1 (low-confidence; left) |
| Everything else | 0 |

The full `lint-c` run (`clang-tidy` on every fork-added `.c` and `.cpp` source
with the new config) emits zero new warnings beyond the pre-existing baseline.
The two `clang-diagnostic-error` lines visible in the run are bash globbing
artefacts (newlines in the file list expansion), not clang-tidy findings.

## Configuration delta

`.clang-tidy` change: swap the two-check whitelist
(`modernize-use-nullptr, modernize-use-override`) for `modernize-*` minus
seven explicit per-check disables. Four are C++-style preferences:

- `-modernize-use-trailing-return-type` — cosmetic; explodes diff size
- `-modernize-use-auto` — opinionated readability trade-off
- `-modernize-avoid-c-arrays` — fights the public `extern "C"` API surface
- `-modernize-use-nodiscard` — noisy on every API function

Three are C-source-noise mitigations (the lint job also parses fork-added
`.c` files, where these checks fire on legitimate C constructs):

- `-modernize-avoid-c-style-cast` — 805 hits on `.c` files where
  `(T)expr` is the only legal cast syntax
- `-modernize-macro-to-enum` — 252 hits on legitimate configuration
  macros (`#define DATA_ALIGN_PINNED 4096`, vector-width constants, …)
- `-modernize-avoid-variadic-functions` — fires on the C printf-style
  wrappers we deliberately keep variadic for ABI compatibility

## Risk

- **CI gate (`lint-and-format.yml` → `clang-tidy` job)**: runs CPU-only
  build with the same `--header-filter` pattern. The four `.cpp` files in
  `build-modernize/compile_commands.json` are the same four files CI lints.
  Verified locally: post-fix run yields zero `modernize-*` warnings in the
  enabled subset.
- **SYCL / CUDA / HIP `.cpp` files**: not in the CPU build's
  `compile_commands.json`, so clang-tidy skips them with a non-fatal warning
  about missing flags (same behaviour as before this PR). Future SYCL CI
  matrix legs may surface new findings; those are addressed when the SYCL
  lint matrix runs, not in this PR.
- **Touched-file rule (ADR-0141)**: this PR's only touched files are
  `feature_collector.cpp`, `metadata_handler.cpp`, and `.clang-tidy`
  itself. All three are lint-clean to the new profile.

## Reproducer

```bash
git worktree add -b chore/clang-tidy-modernize-sweep /tmp/wt-modernize origin/master
cd /tmp/wt-modernize
meson setup build-modernize core -Denable_cuda=false -Denable_sycl=false
ninja -C build-modernize

# Pre-fix baseline
git checkout HEAD~1 -- core/src/feature/feature_collector.cpp \
    core/src/metadata_handler.cpp
clang-tidy -p build-modernize --checks='-*,modernize-*' \
    --header-filter='^core/(src|tools|test)/' \
    core/src/cpu.cpp core/src/feature/feature_collector.cpp \
    core/src/metadata_handler.cpp 2>&1 | grep -oE 'modernize-[a-z-]+' \
    | sort | uniq -c | sort -rn

# Restore + re-run to confirm fixes
git checkout HEAD -- core/src/feature/feature_collector.cpp \
    core/src/metadata_handler.cpp
clang-tidy -p build-modernize --header-filter='^core/(src|tools|test)/' \
    core/src/cpu.cpp core/src/feature/feature_collector.cpp \
    core/src/metadata_handler.cpp 2>&1 | grep -oE 'modernize-[a-z-]+' \
    | sort | uniq -c | sort -rn
```

Expected: pre-fix shows the 5-row table above; post-fix shows only the two
deferred entries.

## Cross-references

- ADR-0915 (this PR) — formal decision.
- ADR-0725 (cpp23-pilot-log-v2) — C → C++23 migration motivation.
- ADR-0278 (nolint-citation-closeout) — every new NOLINT cites an ADR;
  this PR introduces zero new NOLINTs.
- `.clang-tidy` — configuration delta.
