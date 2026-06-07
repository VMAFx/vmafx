<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1068: Fix fast-path data race in gpu_dispatch_env.cpp via atomic publication flag

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `core`, `correctness`, `thread-safety`, `cpp23`

## Context

`gpu_dispatch_env.cpp` (ADR-0858) implements a once-snapshotted environment-variable
cache using a lock-free fast path and a mutex-guarded slow path.  The slow-path writer
populates `slot->var_name` (a `std::string_view`) and `slot->value` (a
`std::optional<std::string>`) under `g_lock`, then returns.  The fast path scans
`g_rows` without any lock, reading `row.var_name` and `row.value` directly.

Under the C++ memory model ([intro.races]), this is a **data race**: the fast-path read
of `row.var_name`/`row.value` and the slow-path write of `slot->var_name`/`slot->value`
are not ordered by any synchronisation operation.  Both `std::string_view` (two-word
pointer+size) and `std::optional<std::string>` (complex object) are multi-word types;
without a happens-before edge the reader may observe a partially-written state.  The
original comment "safe on all coherent ISAs" is correct at the hardware level but
incorrect at the C++ abstract machine level — hardware coherence is a necessary but not
sufficient condition for data-race freedom under the standard's memory model.  TSan
correctly flags this pattern as a race (CWE-362).

The original C implementation (`gpu_dispatch_env.c`) used a pointer-equality fast path
(`g_rows[i].var_name == var_name`) that is effectively safe because pointer assignment
is atomic on all target ISAs, but that idiom does not apply to the C++ port which uses
`std::string_view::operator==` (content comparison requiring both pointer and size to be
consistent).

## Decision

Add a `std::atomic<bool> ready{false}` field to `EnvRow`.  The slow-path writer
populates `var_name` and `value` under the mutex, then publishes with
`slot->ready.store(true, std::memory_order_release)`.  The fast-path reader loads
`row.ready` first with `memory_order_acquire`; only when the result is `true` are
`var_name` and `value` read.  This is the standard C++ publication idiom
([atomics.order] §3): the release store synchronises-with the acquire load, establishing
a happens-before edge that makes all preceding writes (var_name, value) visible to any
thread that observes `ready == true`.

The mutex-guarded slow-path re-check uses `memory_order_relaxed` for the ready flag
because the mutex already provides the necessary happens-before for the non-atomic reads
that follow.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep ADR-0858 status quo ("safe on coherent ISAs") | Zero diff | Data race is UB under C++ standard; TSan flags it; porting risk if compiler reorders around it | Not standard-conforming |
| Remove the fast path — always take the mutex | Trivially correct | Serialises all callers on every log call; GPU backends call this at feature-extractor init — once per frame pair at most, so the cost is negligible | Not chosen on principle of keeping the fast path; the atomic approach costs nothing compared to the mutex acquisition |
| Use `std::shared_mutex` (reader-writer lock) for the fast path | Correct, idiomatic | Heavier than a single atomic bool; `std::shared_mutex` was not present in the prior design | Atomic publication flag is lower overhead and covers the use-case exactly |
| `std::atomic_thread_fence` around existing fields | Avoids adding a field | Fences do not eliminate the UB; the data race on the non-atomic `var_name`/`value` fields remains | Not sufficient |

## Consequences

- **Positive**: data race eliminated; TSan-clean; standard-conforming; fast path retains
  O(kTableCap) lock-free scan with only `kTableCap` atomic loads overhead.
- **Negative**: `EnvRow` grows by one `std::atomic<bool>` (typically 1 byte on x86_64,
  padded to alignment — negligible for an 8-element table).
- **Neutral**: The `constinit` qualifier on `g_rows` is retained (aggregate with
  zero-initialised atomics is constinit-compatible on all supported compilers).

## References

- ADR-0858: original gpu_dispatch_env.cpp C++23 conversion decision
- ADR-0461: caller-contract banning concurrent `setenv("VMAF_*")` during GPU dispatch
- r13 thread-safety audit (2026-06-06): fast-path data race identified
- CWE-362 / SEI CERT CON43-C: "Do not allow data races in multithreaded code"
- C++ standard [atomics.order] §3: release/acquire synchronisation
