# Option sentinel cleanup — 2026-09-08

The full configured CPU lint run at `8100a2caa` found redundant option guards
and loop conditions in `feature_extractor.cpp` and `feature_name.cpp`. Taking
an option-table element's address does not test the table terminator; the old
loops therefore checked `opt->name` separately inside each iteration. The
refactor tests the existing name sentinel directly and scopes the option
pointer to the loop body. A guard after the empty-dictionary early return no
longer repeats the already established count/entry conditions.

Feature-name helper cleanup removes the touched file's existing clang-tidy
debt: private helpers and the dictionary deleter have internal namespace
linkage, the formatting wrapper uses a parameter pack, suffix formatting is
extracted from the allocation helper, and the final ownership guard is const.
The formatting calls, buffer sizes, sorting, aliases, default-value omission,
boolean suffixes and ownership transfers remain equivalent.

This changes no public signature, option, emitted feature key, error path or
GPU dispatch policy. No public-surface/FFmpeg patch update is needed. No new
bug-state row is warranted for this internal cleanup. No new ADR is needed:
there is no policy or architectural decision, and ADR-0729 already owns the
C++ implementation with C linkage. No alternatives: express
the existing terminator and remove redundant internal structure directly.

The read-only extractor argument and feature-name object inputs gain const
pointee qualifiers in their internal declarations and definitions. These
functions are declared under `core/src/feature/`, not the installed public
headers; the exact-symbol inventory found direct consumers and no frozen
callback function type. C and C++ consumers keep passing the same objects.
This resolves the const-parameter findings without a suppression or dummy write.

## Shared pool initialization and cppcheck

An exact-symbol inventory of tracked C/C++ sources found one outer-pool
allocation, in `vmaf_fex_ctx_pool_create()`. `libvmaf.c` and the two existing
pool tests store pointers and call that factory; there are no independent
aggregate/test instances. The factory clears the checked allocation before
setting the thread count and capacity, value-initializes all initial slots,
and initializes its mutex before success. Its allocation/init failure paths
free partial storage and clear the output pointer.

The only slot construction sites are the initial allocation loop in
`vmaf_fex_ctx_pool_create()`, the new-tail loop in `grow_fex_list()`, and the
placement-new reset in `init_fex_list_slot()`. Each uses value initialization.
Slot activation sets the extractor, explicitly stores both atomic counters,
initializes the condition variable, clears the context array and optionally
copies the dictionary. `get_fex_list_entry()` increments the active count
only after activation succeeds; failed slots are not visible to the loops
bounded by that count. Allocation and dictionary-copy failures unwind the
condition/context storage without exposing an active partial slot.

The reviewed `uninitMemberVarNoCtor` declaration warnings therefore describe
missing C++ member initializers, not missing initialization in these factory
paths. Seven inline markers cover only the nine diagnosed member declarations
in these two exact structs. ADR-0772 requires the C-compatible shared layout
and explicit C++ atomic lifetime handling; adding constructors or C++ member
initializers to the C header is not the correction. Other uninitialized-use
checks remain enabled. No global/header wildcard suppression is added, and
the 24 constructor warnings in untouched model/collector headers remain
visible as separate full-gate debt. Preserve the factories and re-review the
markers if any new construction site is introduced.

Validation reuses `test_feature`, `test_feature_extractor` and `test_opt`.
Existing cases cover exact generated names, all option types, aliases, omitted
defaults, null/empty inputs, missing keys, unknown-option rejection and CPU
fallback. No assertion or golden value changes, and no mirror test was added.
Focused lint retains every configured compile-command variant of both source
files; generated build headers and compiler flags come from a separate fresh
CPU release build, with default ASM/LTO and optional backends disabled.

The final run also passed `test_predict`: four existing Meson tests passed
with GCC 15.2.0, Meson 1.10.1 and Ninja 1.13.2. Clang-tidy 22.1.8 measured
zero diagnostics and zero uncited NOLINTs for both C++ sources across four
configured command variants. The scoped CPU baseline writer tightened only
`feature_name.cpp` from seven warnings/two uncited markers to zero, preserving
the full-lane metadata and every other allowance.

Cppcheck 2.21.1 reported no warning/style/error in the four touched source and
header files. Its full invocation still exits 1 on the 24 pre-existing
constructor-model warnings in untouched `model.h` and `feature_collector.h`;
the informational branch-limit/checker reports also remain visible. This is
not a green full-tree lint or GPU/golden-data acceptance claim.

Raw commands, source hashes, tool versions and successful/failed diagnostics
are retained under `.workingdir2/cache/option-sentinel-cleanup-20260908/`.
The original full-gate findings remain in
`.workingdir2/evidence/2026-09-08-rc1-integrated-full-gates-8100a2caa/`.
