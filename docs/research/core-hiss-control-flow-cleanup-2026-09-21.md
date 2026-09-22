# Core HISS control-flow cleanup (2026-09-21)

## Scope

The whole-tree standards baseline still carried seven findings in two core C
files:

- `core/src/feature/y_funque_plus.c`: two `HISS-01` `goto` findings in the
  extractor initializer;
- `core/src/libvmaf.c`: three `HISS-01` `goto` findings in subsystem
  initialization and two `HISS-04` functions over the 60-line limit in the
  CUDA and SYCL picture-ingest paths.

The cleanup removes those findings without changing the baseline, suppressing
an analyzer, or treating upstream-derived code differently. A focused SYCL
clang-tidy pass then exposed one brace-less conditional in the touched
`vmaf_flush_sycl()` function; that finding is fixed in source as part of the
same touched-file contract.

## Implementation and invariants

`y_funque_plus.c::init()` now carries the allocation error through one
structured cleanup block. It keeps the tiny-frame `-EINVAL` return, reports
`-ENOMEM` when either buffer or dictionary allocation fails, and calls
`yf_free_all()` on the same failure paths as before.

`vmaf_ctx_subsystems_init()` uses nested structured acquisition and reverse
destruction. A failure still returns the originating error and tears down only
the subsystems that had initialized successfully.

The oversized GPU functions are divided only at existing execution phases:

- CUDA first drains and collects the previous frame, then opens a new batch
  and submits the current frame;
- SYCL waits for the imported surface, advances the frame/checksum state, then
  collects previous extractor work and submits the current frame.

Extractor order, temporal and subsampling filters, lazy initialization,
`gpu_pending` state, pending indices, CUDA batch open/close order, queue waits,
checksums, and error propagation are unchanged.

## Alternatives considered

| Option | Result | Decision |
| --- | --- | --- |
| Structured cleanup plus phase helpers | Removes all seven findings while preserving existing ownership and ordering | Chosen |
| Add `NOLINT` or baseline entries | Leaves contract violations in touched core code | Rejected |
| Merge CUDA and SYCL into a generic helper | Broadens the change across backend-specific lifetime rules | Rejected |
| Change GPU scheduling while splitting | Couples behavioral work to a standards-only cleanup | Rejected |

No ADR is needed: there is no new architecture or policy decision, only the
one-way implementation of the existing ADR-1142 whole-tree standard.

## Validation

- `standardsctl audit -touched
  core/src/feature/y_funque_plus.c,core/src/libvmaf.c` passes with both files
  clean and active findings reduced from 1,316 to 1,309. No standards baseline
  changed.
- CUDA-configured Clang-Tidy passes for both touched files. The repository's
  SYCL wrapper also passes for `libvmaf.c` under oneAPI 2026.0.
- Cppcheck 2.22.0 with the CI severity set, corrected POSIX model, public-entry
  model, repository suppressions, and the CUDA compile database passes for
  each touched file.
- Semgrep reports zero findings, and `clang-format --dry-run --Werror` passes.
- The CUDA build and the oneAPI SYCL build both compile the changed code.
- Focused runtime binaries all exit zero. Their captured output fingerprints
  after the final source edit are:
  - `test_y_funque_plus`:
    `5e5d7d16c4a60dba0f7c40ed9360a3b2c186f5e577e9c71c0114bb8126a2c9f6`;
  - `test_context`:
    `1b0c46335d6a9b2f678235cd08140f4e3b48dca67607f3c12c9fe02f209d1c12`;
  - `test_thread_safety_batch`:
    `0438685c3e34d1845864bbed4761831825bcd66f18c69881e24e9a6cdaf732c9`;
  - `test_cuda_adm_parity`:
    `8547a649873bfb152dfe7892c4a2146a800ad90acb4cbf0ac02178ef976e8eda`.
- The first three fingerprints are byte-identical to captures taken before
  the refactor. The CUDA parity test also passes; this cleanup does not touch
  its kernel implementation.
- Local Ollama review of the source diff returned `NO FINDINGS`. The preferred
  `vmaf-dev-llm` executable was unavailable, and two `agy` attempts produced
  no review before their bounds expired, so neither was treated as evidence.

The full oneAPI build also printed inherited diagnostics in untouched sources:
floating-point flag overrides, one fallthrough, one missing initializer,
tautological size comparisons, C++ designated-initializer ordering, a doubled
`const`, libsvm override diagnostics, and optimizer unroll remarks. They are
not suppressed or attributed to this refactor; the existing open SYCL lint
sweep remains the durable backlog for those source-level batches.

## Delivery declarations

- Human-facing documentation: no user-visible behavior or public surface
  changes.
- ADR: no new decision; ADR-1142 already governs this cleanup.
- Rebase-sensitive invariant: no new invariant. Preserve the CUDA drain/submit
  order and the SYCL wait/collect/submit order if upstream conflicts touch
  these helper seams.
- Smoke test: build and run `test_y_funque_plus`, `test_context`,
  `test_thread_safety_batch`, and `test_cuda_adm_parity` from a CUDA-enabled
  Meson build.
