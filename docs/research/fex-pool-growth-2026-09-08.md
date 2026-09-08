# Stable feature-context pool entries

## Finding and scope

The compiled internal `vmaf_fex_ctx_pool_aquire()` API keeps an entry pointer
across `pthread_cond_wait()`. That wait releases the pool mutex. Registering a
ninth distinct extractor/options pair can then grow the original eight-entry
table with `realloc()`, moving both the saved pointer and its live condition
variable. A later release signals the new address while the acquisition still
waits on the old address.

At parent `aa2bd08b8`, a synchronized Linux CPU probe completes without growth
but fails to complete after the ninth registration, both with the native
allocator and a link-time wrapper forcing a legal allocation move. The probe
bounds the failed wait at two seconds; it does not use sleeping as readiness.
This is an internal-pool defect: tracked production scoring currently creates
and destroys the pool but does not call its acquire/release operations. An
active scoring hang is not demonstrated.

## Fix and ownership

The expandable table now contains pointers to separately allocated entries.
Growing the table never relocates a published entry, its atomics or its
condition variable. `get_fex_list_entry()` allocates an entry, calls
`init_fex_list_slot()` to value-initialize it, initializes the condition
variable, allocates its context array and copies its options before publishing
the pointer and incrementing `cnt`. Failed initialization frees the unpublished
entry after unwinding any initialized condition variable/context array and
partially copied dictionary. Placement construction is paired with an explicit
`~fex_list_entry()` before freeing an unpublished or destroyed entry.

Table doubling checks both `unsigned` capacity overflow and `size_t` allocation
size. Context-array multiplication is checked separately. Thread counts above
`INT_MAX` cannot be represented by the existing `atomic_int` counters and are
rejected with the same invalid-argument return used for zero; no smaller
operational thread cap is introduced. Registration errors retain the existing
acquire API's `-EINVAL` mapping. Pool destruction releases each options copy
once, including an entry whose first context allocation failed, then destroys
its condition variable and frees the stable entry.

This is an implementation correction under ADR-0772's shared C/C++ lifetime
contract, not a public API or policy change. Replacing the table with another
movable value container would retain the lifetime defect. Holding the pool
mutex through the wait is impossible because release needs the same mutex.

## Regression and analyzer scope

`meson test -C build test_fex_pool_growth test_feature_extractor` runs the
Linux-only link-wrapper regression and existing lifecycle tests. The regression
checks a no-growth control, forced table relocation, and native allocation,
including a failed table reallocation followed by retry. It also injects a
condition-initialization error, four optionless allocation errors, and failures
at all 17 measured allocation sites with two options; each failure must retry
successfully and balance condition initialization/destruction. The `INT_MAX`
thread-count boundary is checked without allocating per-thread contexts.
Its semaphore handshake observes entry into the actual `pthread_cond_wait()`;
the main thread subsequently acquires the pool mutex, proving the waiter has
released it. The original condition-variable address must remain stable.

GNU linker `--wrap` requires externally linked `__real_*` and `__wrap_*`
names. Only those test declarations carry reserved-identifier/linkage or
cppcheck unused-function exceptions: source-only analyzers cannot see the
linker's calls. Production code has no test hooks. GCC may optimize `malloc`
plus clearing
into `calloc`, so both are wrapped; option strings use the wrapped `strdup`.
An early malloc-only fixture missed optimized allocations and is retained as
a failed fixture, not a production failure. C null spelling follows
ADR-1138. CPU validation does not establish GPU execution, Windows support,
Darwin behavior or Netflix Python golden acceptance.

The old-object control linked against unchanged `aa2bd08b8` production objects
fails at its second case, `live entry moved`; only the test's table-access
representation is adapted to the old header. The final CPU release test and
targeted AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer runs pass.
Actual clang-tidy reports zero warnings and zero uncited exceptions. With
cppcheck's official `--library=posix` model, the earlier seven pool-constructor
markers (covering nine diagnostics) were removed after factory-TU analysis.
A subsequent full-profile run identified `fex_ctx_vector.cpp` as a consumer
without visible factory assignments. Because the entry contains self-initializing
`std::atomic` members, this consumer still flags four raw fields: `fex`,
`opts_dict`, `ctx_list` and `full`. Four declaration-only markers now describe
that factory-visibility limitation; atomic and outer-pool markers stay removed.
Placement construction plus explicit stores and `pthread_cond_init` initialize
all four fields before publication. A real-header control reading an uninitialized
entry member remains diagnosed; no uninitialized-use check is disabled. Reduced
analysis context still identifies unused helpers in untouched shared headers;
a whole-tree lint pass is not claimed.

Retained old/fixed commands, source hashes, test logs and analyzer output live
under `.workingdir2/evidence/2026-09-08-fex-pool-growth/`.
