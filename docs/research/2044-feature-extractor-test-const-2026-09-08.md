# Research-2044: Feature-extractor test read-only inputs

The complete CPU-profile receipt at `6d228204f` reports twelve style findings
in `core/test/test_feature_extractor.c`: ten read-only descriptor views, one
read-only empty dictionary view, and the descriptor argument of the private
`fex_vector_create_and_append` helper. A fresh configured build reproduces
all twelve; existing clang-tidy debt is zero.

Those views and the helper argument now point to const. The called
`vmaf_feature_extractor_context_create` and
`vmaf_feature_extractor_supports_options` declarations already accept const
inputs. Context creation copies the descriptor into its own allocation before
mutating it. No production declaration changes are necessary. The mutable
pool-acquire descriptor, fixture objects, context-owned descriptor updates,
dictionary allocation/free operations and backend-gated branches are untouched.
There is no new policy choice or suppression; this implements the existing
read-only contracts.

The original 18 tests pass before and after in a fresh GCC 15.2 CPU release
build with default LTO and assembly enabled. A token comparison permits only
the twelve const insertions and whitespace: all 77 assertion expressions and
messages, all 18 registrations in order, and every other token match. The
linked executable's 934,989-byte `.text` and 92,474-byte `.rodata` sections
are byte-identical before and after.

Actual clang-tidy measures zero warnings, compiler failures and uncited
exceptions. The scoped baseline writer leaves the existing zero baseline
byte-identical. Cppcheck with the official POSIX model reports no finding in
the touched test. Its reduced-context invocation still exits 1 on an unused
inline helper in untouched `feature_collector.h`; this is not a whole-tree
lint pass. All 129 matching native compile variants are retained. GPU build
branches and hardware execution, Windows/Darwin, sanitizer reruns and Netflix
Python golden acceptance are outside this CPU-only result.

Reproduce with `meson test -C build test_feature_extractor`. Exact configure,
analyzer and preservation commands, source/tool hashes, original test,
compiled section comparisons and hook receipts live under
`.workingdir2/evidence/2026-09-08-test-feature-extractor-lint/`.
No public surface, FFmpeg integration or test registration changes occur.
There is no new rebase-sensitive invariant beyond preserving the existing
test assertions and fixture ownership.
