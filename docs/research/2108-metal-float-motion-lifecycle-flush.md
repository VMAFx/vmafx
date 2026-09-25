<!-- markdownlint-disable MD013 -->
# Research-2108: Metal float-motion lifecycle, debug gating, and flush regression — 2026-09-25

## Finding

Canonical BUG048 section A5 covers float-motion lifecycle correctness, option
parity, and flush idempotency across GPU backends. An audit of
`core/src/feature/metal/float_motion_metal.mm` against authoritative CPU, CUDA,
SYCL, and HIP implementations identified four correctness gaps:

1. **Option registration and alias drift**: Options `debug` (default `true`)
   and `motion_force_zero` (alias `force_0`, default `false`, marked
   `VMAF_OPT_FLAG_FEATURE_PARAM`) were not registered in `options[]`, causing
   option validation to reject them or ignore them during feature configuration.
2. **Missing force-zero bypass and dictionary leak risk**: There was no
   `extract_force_zero_metal` path. Cloned extractors with
   `motion_force_zero=true` need to release early device resources while
   retaining the dictionary-owning destructor (`fex->close = close_fex_metal`)
   to avoid leaking `feature_name_dict`.
3. **Unconditional debug score emission**: `collect_fex_metal()` appended
   `VMAF_feature_motion_score` unconditionally instead of gating it behind
   `if (s->debug)`.
4. **Flush non-idempotency**: `flush_fex_metal()` appended the tail `motion2`
   sample without verifying if the tail had already been written, causing
   subsequent flushes to return collector append errors. Furthermore, querying
   only the literal base name misses option-derived names when
   `motion_fps_weight` is set.
5. **Control flow compliance (HISS-01)**: `init_fex_metal()` previously used
   `goto` cleanup ladders.

## Architecture and Lifecycle

`init_fex_metal()` now checks `s->motion_force_zero`:

- When true, it calls `init_force_zero_metal()`, which builds
  `feature_name_dict`, installs `extract_force_zero_metal`, and retains
  `fex->close = close_fex_metal`.
- When false, it allocates device resources via `fm_metal_init_device()`. If
  device allocation fails, it cleans up null-safely via `fm_metal_release()`
  without any `goto` statement.

In `collect_fex_metal()`:

- `VMAF_feature_motion_score` is emitted only when `s->debug` is true.

In `flush_fex_metal()`:

- Early return if `s->frame_index == 0u`.
- Resolves the published score name via `vmaf_dictionary_get(&s->feature_name_dict,
  VMAF_feature_motion2_score, ...)`.
- Probes `vmaf_feature_collector_get_score()`: if the score is already present
  at `s->frame_index`, it returns `1` immediately without re-appending,
  achieving idempotency.

## Decision Matrix

| Option | Resource safety | Flush coverage | Decision |
| --- | --- | --- | --- |
| Keep unhandled options and unconditional append | Option rejection; duplicate append fails | Repeated flush fails | Rejected: violates BUG-048 A5 |
| Set `fex->close = NULL` on force-zero path | Leaks `feature_name_dict` on context destruction | N/A | Rejected: leaks allocated dictionary |
| Probe literal name in flush | Fails when option parameters (e.g. `motion_fps_weight`) alter feature key | Flushes fail under non-default weights | Rejected: fragile under parameterization |
| Retain `close_fex_metal`, gate debug, probe dictionary-resolved name | Complete memory safety, HISS-01 compliant, full option & flush parity | Idempotent under all options | Chosen |

## Verification Evidence

1. **Source Contract Test** (`core/test/test_metal_float_motion_contract.py`):
   - AST inspection of `core/src/feature/metal/float_motion_metal.mm`.
   - Verified struct fields `debug` and `motion_force_zero`.
   - Verified option table registrations and alias `force_0`.
   - Verified `init_force_zero_metal` retains `close_fex_metal`.
   - Verified debug gating in `collect_fex_metal`.
   - Verified flush idempotency with dictionary lookup.
   - Verified zero `goto` statements in `init_fex_metal`.
2. **GPU Option Alias Contract** (`core/test/test_gpu_option_alias_contract.py`):
   - Verified `("metal/float_motion_metal.mm", "motion_force_zero", "force_0")`
     alias parity.
3. **Parity Harness** (`core/test/test_metal_float_motion_parity.c`):
   - Added force-zero close destructor retention probe and flush idempotency
     probe.
   - Cleanly skips on non-Apple-Silicon platforms (`[skip: no Metal device]`).
4. **Standards & Compliance**:
   - `praetorctl audit` verified pass with 0 warnings on touched files.
   - Zero modifications to Netflix golden assertion constants.
