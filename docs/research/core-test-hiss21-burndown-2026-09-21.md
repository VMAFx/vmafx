# core/test HISS-21 burn-down (2026-09-21)

## Scope

`praetorctl audit` reported 53 active HISS findings under `core/test/`
across 39 files: two HISS-02 unbounded loops, four HISS-01 `goto` jumps,
and 47 HISS-04 blocks over the 60-line cap. This pass clears 52 of them
in source. The whole-repository count moves from 1,063 to 1,011 active
violations. No baseline file, analyzer suppression, threshold, assertion
value or test registration changed.

## What the 60-line cap actually measures

The scanner is a brace-tracked block-length check, not a parser. It
counts every brace block that opens at file scope, so three shapes in
this tree are reported as `Function '{'` although no function is
involved:

- an anonymous `namespace` whose opening brace `clang-format`'s
  `BreakBeforeBraces: Linux` puts on its own line
  (`test_feature.cpp`, `test_fex_ctx_vector.cpp`,
  `test_fex_ctx_vector_public.cpp`, `test_model_registration_ownership.cpp`,
  `test_registration_partial_copy.cpp`, `test_spinner.cpp`);
- a long table initializer (`test_model.c`'s 62-row case table);
- any function whose signature wraps onto a second line.

Putting the namespace brace on the same line does not help — the check
still counts the block. Replacing the namespace with `static` is not
available either: `misc-use-anonymous-namespace` is enabled in
`.clang-tidy`, so that trades one finding for a new lint warning. The
fork already settled this shape elsewhere: the SYCL extractors and
`core/tools/vmaf.cpp` use several short, reopened
`namespace { ... } // namespace` blocks, one per cohesive group, and
`docs/rebase-notes.md` records the reason. The C++ tests now follow that
same convention, and `test_model.c`'s table becomes four ordered
`MuTest` tables driven by the shared `mu_run_table()` from
`mu_table.h`, replacing that file's private copy of the same runner.

## Implementation and invariants

**Bounded loops (HISS-02).** `_sha256()` in `test_vmaf_tiny_v2.py` and
`copy_file_600()` in `test_vmaf_use_tiny_model.c` both read until EOF.
Each now derives its iteration count from the size measured before the
first read — `size // chunk + 1` and `ceil(size / sizeof(buf)) + 1` —
and treats bound exhaustion as an error rather than as completion: the
Python helper raises, and the C helper leaves `rc` at `-1` because only
the end-of-file iteration sets it to `0`. A fixture that grows mid-copy
now fails instead of yielding a truncated destination.

**Acyclic cleanup (HISS-01).** `compare_geometry()` in
`test_float_adm_dwt2_neon.c` carried four `goto out` jumps into one
free-everything epilogue. Allocation now lives in
`dwt_buffers_alloc()`, which returns the `mu_assert` message and leaves
whatever it managed to allocate for the caller's unconditional
`dwt_buffers_free()`. The comparison work moves into `run_and_tally()`,
so `compare_geometry()` is allocate / run / free with a single exit.
Poison fill order, the signed-zero fixture, the mismatch counters and
every `fprintf` format string are unchanged.

**Over-long bodies (HISS-04).** Splits follow existing execution phases
and propagate failures with `mu_assert_msg()`, a one-line form of the
`char *msg = check_x(...); if (msg) return msg;` idiom that
`core/test/AGENTS.md` already prescribes. It is defined once in
`test.h`; `test_pool_percentile.c`'s private copy is removed rather than
duplicated. Three classes of split carried a real risk and were handled
explicitly:

- *Accumulator order.* No helper returns a per-block partial sum. Where
  a loop feeds a running total, the total is passed by pointer so the
  sequence of additions is byte-identical (`tally_band_values()`,
  `fill_random_plane()`, `append_all_features()`).
- *Shared device state.* `run_sycl_motion_uv()` previously ran two
  passes over one `VmafSyclState` in two brace blocks, with a comment
  arguing that splitting would obscure ownership. The state is now
  initialised and released by the driver and borrowed by
  `run_sycl_pass_add_uv()` / `run_sycl_pass_y_only()`, which is the same
  single lifetime. The driver also releases it when a pass fails, where
  the old code returned without freeing.
- *Duplicated drivers.* The three CUDA preallocation cases differed
  only in `pic_prealloc_method`; they now share
  `run_preallocation_method()`. Twelve HIP parity tests repeated the
  same `-ENOSYS` scaffold teardown; it is now
  `hip_parity_skip()` in `core/test/hip_parity_skip.h`, whose `where`
  argument reproduces each site's message verbatim.

Three `NOLINTNEXTLINE(readability-function-size)` comments became
untrue once their functions were split and were removed with the split
(`test_sycl_motion3_parity.c`, `test_sycl_motion_add_uv_parity.c`,
`test_ssimulacra2_simd.c`, and the upstream-parity one in
`test_barten_csf.c`). No NOLINT was added.

## Deliberate residue

`ref_calc_psnrhvs()` in `test_psnr_hvs_simd.c` (115 lines) is left
exactly as it is. It is a transliteration of the upstream scalar
PSNR-HVS that the AVX2 and AVX-512 kernels are checked against, so its
structure is the contract; the in-source comment records that an earlier
attempt to split the per-block body changed the accumulation and
produced `scalar=inf`. The only remaining way to reach 60 lines is to
move the cross-block `ret` / `pixels` accumulation across a function
boundary, which is precisely what CLAUDE.md §12 r12 names as the
sanctioned exception ("an ADR-0138 / ADR-0139 bit-exactness pattern").
The file is therefore not touched at all, so the touched-file clean rule
does not apply to it and the existing cited NOLINT stands.

Two pre-existing oddities were observed and deliberately not changed,
because removing them is capability deletion rather than a standards
fix: every HIP parity test has a second, unreachable
`if (err == -ENOSYS)` block after the first one returns, and
`test_hip_float_ssim_parity.c` reaches its scaffold branch only through
that dead path.

## Alternatives considered

| Option | Result | Decision |
| --- | --- | --- |
| Phase helpers plus shared skip/driver helpers | Clears 52 findings, keeps every assertion and message | Chosen |
| Baseline entries or `// NOLINT` for the block cap | Leaves the contract violated in fork-local test code | Rejected |
| Replace anonymous namespaces with `static` | Trades HISS-04 for `misc-use-anonymous-namespace` warnings | Rejected |
| `namespace {` on one line | Scanner still counts the block; `clang-format` reverts it | Rejected |
| Split `ref_calc_psnrhvs()` | Risks the ADR-0138 bit-exactness contract on lanes that cannot be tested here | Rejected |
| Delete the unreachable HIP `-ENOSYS` blocks | Shrinks the bodies by deleting code | Rejected |

No ADR is needed: this is the one-way implementation of the existing
ADR-1142 whole-tree standard, with no new architecture or policy.

## Validation

- `praetorctl audit` in the worktree: `HISS invariant scan verified:
  1011 active violations within 1411 baselined limit (41 touched files
  clean)`, down from 1,063 on the branch point. No baseline file
  changed.
- `meson setup build-hiss core -Denable_cuda=false -Denable_sycl=false`
  then `ninja -C build-hiss`: clean, zero compiler warnings.
- `meson test -C build-hiss`: `Ok: 157, Fail: 0` — the same 157 tests
  that pass on the branch point.
- Backend-gated files that the CPU build does not compile were checked
  with `gcc -fsyntax-only -std=gnu2x -Wall -Wextra`: the twelve HIP
  parity tests, the two SYCL parity tests and the SYCL CAMBI smoke test
  against the repository headers; the three CUDA tests against
  `/opt/cuda/include` with `HAVE_CUDA` defined; and the libFuzzer target
  with `_GNU_SOURCE`. All clean.
- The three aarch64-only tests (`test_float_adm_dwt2_neon.c`,
  `test_motion_neon.c`, `test_ssim_neon.c`) were checked with
  `aarch64-linux-gnu-gcc -fsyntax-only -Wall -Wextra` against a
  config header with `ARCH_AARCH64 1`. All clean.
- `clang-format --dry-run --Werror` passes for every touched C/C++ file;
  `ruff check` and `black --check` pass for both touched Python files.
- `python3 -m unittest` on `test_windows_cuda_compiler_discovery.py`:
  4 tests, all pass — it configures four real Meson projects, so the
  refactored fixture builder is exercised end to end.
- `build-hiss/test/test_ssimulacra2_simd` passes all 13 cases, which is
  the byte-exact `memcmp` of the scalar reference against the dispatched
  SIMD kernel: the constant-lifting in `ref_picture_to_linear_rgb()` and
  the shared `fill_random_plane()` changed no bit of its output.
- A second build with `-Db_lto=false` (the shape the clang-tidy lane and
  the allocation-injection harness need) runs `Ok: 159, Fail: 0`. The two
  extra cases are `test_fex_ctx_vector`'s `FEX_VECTOR_ALLOC_TEST`
  variants, which the default LTO build compiles out, so the reopened
  anonymous namespaces inside that `#ifdef` are covered as well.

## clang-tidy ratchet

Splitting a body adds functions, and in a file that still carries
`modernize-use-nullptr` or `misc-use-anonymous-namespace` debt every added
function raises that file's clang-tidy count. Each touched file was
therefore measured against `scripts/ci/tidy-baseline-cpu.json` with
`scripts/ci/tidy-ratchet.py --only`, and each new helper was written so it
adds no warning of its own:

- assertion-heavy helpers are driven from a case table, so their branch
  count does not grow with the case count (`check_csf_cases()`,
  `check_piecewise_cases()`, the two tables in
  `test_find_linear_function_parameters()`);
- the new C++ helpers in `test_dict.cpp` take internal linkage from short
  anonymous namespaces and spell null as `nullptr`, which is what
  clang-tidy asks of a C++ TU;
- `set_meta()` in `test_predict.c` became `static`, and `PiecewiseCase`
  is field-ordered so it carries no padding.

The result is zero regressions. `core/test/test_barten_csf.c` measures 0
against a baseline of 0, `core/test/test_predict.c` 13 against 13,
`core/test/test_pelorus_interop.c` 9 against 9, `core/test/test.h` 2
against 2, and every other touched file 0 against 0.
`core/test/test_dict.cpp` measures 31 against a baseline of 33, because
the four helpers it gained are clean while two of the constructs they
replaced were not. That is slack, not a regression: the ratchet asks for
a scoped tightening,

```bash
python3 scripts/ci/tidy-ratchet.py --lane cpu --write \
    --only core/test/test_dict.cpp
```

run from a toolchain matching the baseline's recorded
`gcc-15 (Ubuntu 15.2.0-16ubuntu1)`. This pass does not write it: a
baseline is not edited from a machine whose measurement cannot be
compared with the one the baseline records.
