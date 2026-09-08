# Configured native lint selection — 2026-09-08

The local `make lint-c` driver at `cc022245079f1b7da4005a39bcefc759e2d6ef3d`
overwrites Meson's database with unfiltered `ninja -t compdb`, then selects
sources using `core/src/**/*.c`, `core/src/**/*.cpp` and `core/tools/*.c`. The
first two Git pathspecs omit engine-root files; the third omits C++ tools.
Tests are absent. Conversely, inactive ARM and CUDA files are selected without
configured commands, producing unrelated inferred-command diagnostics.

The retained CPU profile contained 1,215 native Meson command records. Ninja's
unfiltered export expanded that to 1,411 records including generation/link
commands. The Make source list selected 216 files: 99 had no configured entry,
while 157 configured tracked sources were omitted. These are profile-specific
counts, not the canonical hosted GCC14 ratchet inventory. The full lint
attempt was interrupted after confirming driver defects, and is not a passing
receipt.

The first observed clang parser failure was GCC `-flto=4`. The existing hosted
lint job already avoids it with a separate `-Db_lto=false` profile. A private
analyzer database instead maps positive numeric `-flto=N` to `-flto`,
preserving the original build's configuration and byte-identical input
database. Existing clang spellings and unknown values are not normalized into
success.

The implementation follows ADR-1142's configured-source coverage policy. No
new ADR is needed: this repairs the local selector without changing the
ratchet baselines, diagnostic thresholds or remote job scope. The existing
ratchet loader selects source paths from its database, then clang-tidy reads
all matching commands; selecting a source once must not discard distinct
macro/include/language variants and optional output metadata. Local lint also
retains tracked vendored sources. Generated/untracked sources and inactive
tracked sources are recorded separately; the ratchet's generated-source policy
remains independent.

Alternatives: adding globs leaves inactive files eligible and misses future
extensions; selecting one command per filename loses meaningful test variants;
reconfiguring the user's build changes its requested profile. The selected
approach keeps the native database and filters a private copy against Git's
tracked native paths, preserving all matching entries. Both analyzers run so
one failure does not hide the other's findings.

Validation uses scratch Git repositories and real subprocess boundaries with
analyzer stubs, including the actual Make target. Controls cover root engine,
C++ tools, tests, tracked vendor and CUDA sources; inactive ARM and generated
inputs; distinct macro variants; original database preservation; command and
arguments schema; numeric-only LTO adaptation; missing/invalid inputs; and
independent analyzer failures. These are driver regressions, not native
runtime or numerical tests. Netflix golden assertions are untouched.

Retained machine evidence:
`.workingdir2/cache/rc1-full-lint-20260908-cc022245/` contains both
interrupted full-Make attempts, tool/image hashes, command logs,
Meson/Ninja/analyzer help, and the scope inventory. The original 134/134 CPU
test receipt is under
`.workingdir2/evidence/2026-09-08-rc1-full-gates-cc022245/`; it does not
establish golden, sanitizer or GPU acceptance.

The [LLVM compilation database
specification](https://clang.llvm.org/docs/JSONCompilationDatabase.html)
permits multiple commands for a source and an optional `output` field that
distinguishes processing modes. The analyzer copy retains this metadata and
validates its type alongside the command schema.

Migration check: the actual 1,411-entry export contains all 1,215 native
commands unchanged plus 196 non-native extras. The 48–51 DNN variants are
genuine test configurations already in the native database. Ten extras are
phony entries with empty commands; strict schema validation rejects them. In a
disposable copy, `meson setup --reconfigure BUILD SOURCE/core` restored 1,215
entries without changing the JSON build-options inventory. Make runs this
no-override reconfigure before building and analyzing. The helper never
rewrites the input database itself. A no-op-build fixture verifies old phony
entries are repaired; ordinary reconfiguration does not discard genuine
source-command variants.
