# Research-2041: Thread-pool backpressure and shutdown lifetime

- **Status**: Active
- **Workstream**: ADR-1243; Netflix/vmaf #1587 queue admission fix
- **Last updated**: 2026-09-08

## Question

Does the upstream queue bound apply to VMAFx, and how can admission and shutdown
preserve the fork's recycled jobs, worker data and batch-error semantics?

## Source evidence

Netflix's [8fc71e3006f0b21e8e31d6e5d1b904332149ad9e](https://github.com/Netflix/vmaf/commit/8fc71e3006f0b21e8e31d6e5d1b904332149ad9e)
adds a queue count, waits at the worker-count capacity and wakes producers on
dequeue. The change addresses the mechanism described in
[issue 1587](https://github.com/Netflix/vmaf/issues/1587): input arriving faster
than scoring retains pending pictures without a queue bound.

The fork at `9f5cb1900abb1c8101481ba587a32c7fc1bc6b73` lacks that admission wait.
Its additional job recycling does not limit the active queue; the retained
payloads can still grow with producer/consumer imbalance.

## Adaptation

Apply admission before job/payload allocation, preserving the inline/heap
cleanup distinction, two-argument callbacks, per-worker private data and
mutex-protected error OR/reset. The immutable count of successfully created
workers sets capacity, including partial `pthread_create` failure.

The upstream shutdown broadcast alone does not protect blocked producers:
its stop wait counts workers, and a final worker can exit before a woken
producer reacquires the queue mutex. The fork also counts admitted producers
until they leave the wait; destruction retains the condition/mutex until both
workers and those producers have exited. This is not an arbitrary concurrent
entry/destruction protocol: registration happens after acquiring the queue
mutex. The owner must serialize destruction against API entry, including
callers waiting for that mutex. The cancellation test establishes an external
condition-wait barrier; ordinary producer stress joins all entrants first.

The new condition is included in every checked-init unwind and total
thread-creation failure cleanup. No new scheduling option or numerical policy
is introduced, so no new ADR is needed. These are queue/lifetime corrections
to the existing ADR-0147 recycling and checked-init contracts.

## Reproduction and limits

`test_thread_pool_backpressure.c` fails against the original fork source:
its producer completes admission instead of reaching the capacity wait.
Removing only the producer-lifetime term from the adapted stop-wait predicate
also triggers ASan heap-use-after-free in the delayed-producer case. The
adapted source passes that test and the delayed-producer shutdown, concurrent
payload/error recycling and initialization failure cases. The test includes
the implementation TU only in its own executable to inject pthread failures
and a delayed reacquisition deterministically; it does not also link the TU
or libvmaf. This avoids platform-specific linker interposition (ADR-0141).

The bounded local checks compile current worktree sources, never an installed
libvmaf binary. A separate container with two CPUs and no network or devices
runs ASan/UBSan and TSan variants. The normal Meson CPU configuration registers
both thread-pool executables in the fast suite. Full-scoring, golden and
platform-wide validation remain separate integration gates; the upstream
reporter's macOS/VideoToolbox workload was not reproduced here.

## Lint baseline measurement

The isolated CPU Meson database has 253 unique translation units, while the
committed full CPU report has 292. Its Clang CPU configuration disables assembly
and DNN dependencies, so it cannot replace the CI GCC 14 database. The local
clang-tidy executable is LLVM 22.1.8, matching the committed report.

ADR-1243 adds guarded scoped tightening instead of hand-editing that report.
The existing Meson commands for the two changed translation units are retained
with container `/source` and `/out/build` paths mapped to their identical host
mounts. clang-tidy measures both at zero warnings and zero uncited NOLINTs.
The writer preserves every unmeasured entry and the original full-report
metadata; only `core/src/thread_pool.c` tightens from 15 warnings to zero.
Filesystem fixtures cover report aliases, failed atomic replacement, empty or
inexact coverage, errors, unreadable sources/headers, debt increases, unchanged
headers and zero tightening. Two actual subprocess writers prove full/scoped
lock contention fails without output; separate fixtures preserve a concurrent
outside edit detected before replacement and reject drift during measurement.
