# core/test HISS-21 burn-down (2026-09-21)

## Scope

`praetorctl audit` reported 53 active HISS findings under `core/test/`
across 39 files: two HISS-02 unbounded loops, four HISS-01 `goto` jumps,
and 47 HISS-04 blocks over the 60-line cap. This pass clears 51 of them
in source. The whole-repository count moves from 1,063 to 1,012 active
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

Five `NOLINTNEXTLINE(readability-function-size)` comments in three
fork-local files became untrue once their functions were split, and were
removed with the split. `core/test/` holds 40 citations for this check
before the pass and 35 after; the five are:

- two in `test_ssimulacra2_simd.c` and one in
  `test_sycl_motion3_parity.c`, whose text was a bare category — "test
  scaffolding (ADR-0141)" and "test harness setup and per-frame loop" —
  and named no invariant at all;
- the two stacked over `run_sycl_motion_uv()` in
  `test_sycl_motion_add_uv_parity.c`: one claiming that splitting "hides
  which assertion fired", one that it would force the opaque `sycl_state`
  pointer through the `mu_assert` return protocol and obscure the
  ownership model.

The first three state no claim to satisfy. The two over
`run_sycl_motion_uv()` do, and both are refuted by the split that
replaced them: every `mu_assert` string is byte-identical and is returned
to the caller through `mu_assert_msg()`, so the failing stage still names
itself, and `sycl_state` is still initialised once and released after
both passes in `run_sycl_motion_uv()`, which now says so in one sentence
instead of relying on the reader inferring it from a single long body.
None of the three files exists upstream (`git cat-file -e
upstream/master:libvmaf/test/<file>` fails for each), so no sync story
depends on their shape. The boundary this draws — a cited suppression may
be retired only when the refactor that replaces it demonstrably satisfies
the invariant the citation names — is recorded in
[ADR-1286](../adr/1286-retiring-cited-lint-suppressions.md). No NOLINT
was added.

The one cited suppression this pass leaves in place is in
`test_barten_csf.c`; see *Deliberate residue*.

## Deliberate residue

`test_barten_csf()` in `test_barten_csf.c` (91 lines) is left exactly as
it is, and keeps its
`// NOLINTNEXTLINE(readability-function-size) — ADR-0141 §2
upstream-parity invariant; ADR-0278`. The body is Netflix upstream's own
`mu_assert(almost_equal(...))` sequence carried verbatim from `c70debb1`
(`docs/rebase-notes.md` records the port as "verbatim from upstream"),
and upstream keeps appending cases to it — `c2155d6cd` added the 2160p
CSF rows the fork then took. Reshaping those 40 statements into case
tables would satisfy the 60-line cap, but it would also make every later
upstream sync of this file a hand-merge instead of a diff-and-merge,
which is precisely the invariant the citation names. An earlier revision
of this pass did split it; the split was dropped before the branch
settled, because the file is the one place in this sweep where the
suppression is load-bearing under ADR-0141 §2 rather than a category
label. The file therefore carries no hunk against the merge base at all.

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

The result is zero regressions. `core/test/test_predict.c` 13 against 13,
`core/test/test_pelorus_interop.c` 9 against 9, `core/test/test.h` 2
against 2, and every other touched file 0 against 0.

Four touched files came out *below* their baseline, which the ratchet
treats as a failure of its own (exit 3) until the baseline is tightened
in the same change:

- `core/test/test_dict.cpp`, 33 -> 31. The four helpers it gained are
  clean while two of the constructs they replaced were not: one
  `readability-function-size` (5 -> 4) and one `modernize-use-nullptr`
  (21 -> 20).
- `core/test/test_pic_preallocation.c`, 16 -> 10, from the earlier
  `fix(core): clear high-signal native diagnostics` commit on this
  branch. Six diagnostics went with that cleanup and the baseline had
  not followed.
- `core/src/framesync.c`, 7 -> 0, from
  `fix(ci): correct pthread modeling and init failures`.
- `core/test/test_framesync.c`, 8 -> 0, from
  `test(core): propagate teardown failures`.

All four were tightened with the scoped form, which rewrites only the
named TU's allowance and keeps the rest of the last full measurement
(ADR-1243):

```bash
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
    --clang-tidy /usr/bin/clang-tidy --write \
    --only core/test/test_dict.cpp
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
    --clang-tidy /usr/bin/clang-tidy --write \
    --only core/test/test_pic_preallocation.c
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
    --clang-tidy /usr/bin/clang-tidy --write \
    --only core/src/framesync.c --only core/test/test_framesync.c
```

The baseline records two toolchain fields, and this machine matches one
of them: `clang_tidy_version` is `22.1.8` here and in the baseline, but
`cc_version` is `cc (GCC) 16.2.1` against the baseline's
`gcc-15 (Ubuntu 15.2.0-16ubuntu1) 15.2.0`. ADR-1230 records why the
second field exists — clang-tidy parses each TU against the C compiler's
system headers, so a count can move with gcc alone — so the mismatch was
treated as a hypothesis to falsify rather than as a blocker, and
falsified per TU before either write:

1. Restore the merge-base content of the file, measure it here, and
   compare with the committed baseline. `test_dict.cpp` measured 33
   against a baseline of 33 (21 `modernize-use-nullptr` + 7
   `misc-use-anonymous-namespace` + 5 `readability-function-size`, the
   same distribution the baseline records); `test_pic_preallocation.c`
   measured 16 against 16, `framesync.c` 7 against 7 and
   `test_framesync.c` 8 against 8, each with a *byte-identical*
   diagnostic list, line and column included.
2. Check the headers each TU drags in, which is the surface ADR-1230
   warns about. `core/include/libvmaf/feature.h` 1, `core/src/dict.h` 4,
   `core/src/dict_internal.h` 2 and `core/test/test.h` 2 all matched the
   baseline exactly.

The measurement is therefore reproducible here for these four TUs
notwithstanding the gcc difference, and the four deltas are attributable
to this branch rather than to the toolchain. Every diagnostic behind
them is in the fork's own C/C++, not in a system header.

Re-measuring all 37 touched TUs against the tightened baseline leaves no
source file above or below its allowance. Compare over the union of the
touched list and the measured paths, not over the measured paths alone:
a file that measures 0 carries no entry in the measurement's `warnings`
map, so iterating that map silently skips exactly the files that were
cleaned to zero — which is how `framesync.c` and `test_framesync.c` were
missed on the first pass.

Several headers read one or two below theirs. A 37-TU measurement cannot
tell a real reduction from a diagnostic whose only reporting TU is
outside the scoped set, so the whole lane was measured instead — 316 TUs,
zero compile failures — and the headers separate into two groups.

Two are real, and both belong to this branch. `core/src/log.h` reads 0
against a baseline of 1 and `core/src/framesync.h` reads 1 against 2; the
diagnostic missing from each is
`bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp` on the
`__VMAF_SRC_LOG_H__` and `__VMAF_FRAME_SYNC_H__` include guards, which
`fix(core): make C23 logging analyzer-clean` and `fix(ci): correct pthread
modeling and init failures` renamed to `VMAF_SRC_LOG_H_` and
`VMAF_FRAME_SYNC_H_`. Measuring `origin/master` on the same machine
reproduces both diagnostics at `log.h:20:9` and `framesync.h:20:9`, so the
reduction is the branch's, not the toolchain's.

Neither can be written from this machine, and that is *Deliberate
residue*, not an oversight. The scoped writer (ADR-1243) rewrites only the
TU paths passed to `--only`, and a header is not a TU, so a header
allowance moves only under a full `--write`. A full write replaces every
entry with this machine's measurement, and this machine is not the one the
baseline records: `cc_version` here is `cc (GCC) 16.2.1` against the
baseline's `gcc-15 (Ubuntu 15.2.0-16ubuntu1)`, and the difference is not
theoretical — `core/src/dict.cpp` measures 16 against a baseline of 15 and
`core/src/feature/feature_collector.cpp` 16 against 13, on **`origin/master`
as well as on this branch**, from `misc-const-correctness`,
`misc-use-anonymous-namespace` and `bugprone-suspicious-stringview-data-usage`
that gcc-16's libstdc++ headers produce and gcc-15's do not. Writing those
three hundred entries to buy two header lines would trade a two-line
overstatement for a tree-wide one. `lint-and-format.yml` builds this lane
with `CC=gcc-15 CXX=g++-15` and measures it with `/usr/bin/clang-tidy-22`,
and uploads the result as `tidy-ratchet-cpu.json` precisely so it can be
committed as the new baseline; that artifact, not a local write, is where
these two lines come from. The dev container has the gcc-15 half and no
clang-tidy-22; this host has the clang-tidy-22 half and no gcc-15.
