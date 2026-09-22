<!-- markdownlint-disable MD013 MD018 MD060 -->
# Research-0064: GPU-lane NOLINT audit — which suppressions are actually live

- **Date**: 2026-09-22
- **Owner**: Lusoris
- **Tags**: lint, audit, gpu, sycl, cuda, hip, metal, touched-file-rule
- **Companion ADR**: [ADR-1290](../adr/1290-sycl-tidy-lane-compile-database.md)
- **Ledger**: `.workingdir/BUGS.md` `BUG-029`, `BUG-041`

## Question

[ADR-0141](../adr/0141-touched-file-cleanup-rule.md) §2 allows a `NOLINT` only
where refactoring would break a load-bearing invariant, and requires an inline
`ADR-NNNN` citation naming it. [ADR-0278](../adr/0278-t7-5-nolint-sweep.md)
discharged that for the CPU lane. The GPU lane was never swept. **How many
uncited markers are there, and — the question the earlier attempts skipped —
which of them actually suppress a diagnostic?**

Citing a marker that suppresses nothing is worse than leaving it: it
manufactures a justification for a suppression the code does not need, and the
citation makes it look audited.

## How many

`count_uncited_nolints()` in `scripts/ci/tidy-ratchet.py` is the function the
ratchet gates on, so it is the authority. On `fb06193b3`, tree-wide:

| Source | Count | Note |
|---|---|---|
| Filed in `BUGS.md` (headline) | 50 | Matches no counting method the tool implements |
| Filed in `BUGS.md` (legacy note) | 6 | Correct *for what the lanes could see* |
| Sum of the lane baselines | 6 | cuda 3 + hip 1 + sycl 2 |
| `count_uncited_nolints()`, tree-wide | **21** | The real figure |

The 15-marker gap is exactly the `BUG-041` blind spot: 14 markers under
`core/src/feature/sycl/` + `core/src/sycl/` and 1 under
`core/src/feature/metal/` live in files **no lane measures** — the sycl lane's
compilation database contained zero SYCL feature TUs, and there is no Metal
lane. A gate that cannot see a file reports it as clean.

A same-line grep (`NOLINT` without `ADR-` on that line) over the GPU trees
returns 246 of 468 markers, which is presumably where a figure like "50" came
from; it is not what the tool measures, because a citation also counts on the
previous or next line and inside the enclosing block comment.

## Method

For each file, every `NOLINT*` token was rewritten to an inert spelling and
clang-tidy re-run over the real compilation database for that lane
(`--cuda-host-only -nocudalib` for cuda, `-x hip -D__HIP_PLATFORM_AMD__=1` for
hip, `scripts/ci/clang-tidy-sycl.sh` for sycl), then the file restored. A
marker whose check appears in the diagnostic set is **live**; one whose check
does not is **dead**.

Two traps worth recording:

1. **Parse the whole bracket.** Diagnostics carry every enabled alias, e.g.
   `[performance-no-int-to-ptr,-warnings-as-errors]`. A regex anchored on
   `[a-z-]+\]` captures only the last token and silently reports the check as
   absent — which is how a first pass wrongly concluded the two CUDA
   Driver-API brackets were dead.
2. **Match `error:` as well as `warning:`.** The ten checks in `.clang-tidy`'s
   `WarningsAsErrors` list — `performance-no-int-to-ptr` among them — are
   reported at error level.

Control: a probe TU confirms `misc-const-correctness` fires under this exact
config, so its absence on the SYCL kernels is a real negative, not a disabled
check.

## Result

21 markers: **7 dead**, **1 live but misdescribed**, **13 live and load-bearing**.

### Dead — removed, not cited

| Site | Check | Why it never fires |
|---|---|---|
| `cuda/ssimulacra2_cuda.c:301` | `readability-function-size` | Guards `ss2c_setup_gaussian`, which is under the thresholds. Its siblings at 406 / 905 / 1006 do exceed them and are cited — so the ledger's "copy theirs" advice would have cemented a false citation |
| `sycl/integer_adm_sycl.cpp:480` | `bugprone-branch-clone` | The two arms are not clones: one reads `uint8_t` at `e_in_stride`, the other `uint16_t` at `e_in_stride / 2` |
| `sycl/integer_adm_sycl.cpp` ×2 | `misc-const-correctness` | On `csf_accum_ptr` / `cm_accum_ptr`. The check proposes a **`const` pointee**; the pointee is mutated through `atomic_ref`, so the check correctly stays silent |
| `sycl/integer_adm_sycl.cpp` ×2 | `misc-const-correctness` | On `total_csf` / `total_cm`, which the following loop accumulates into |
| `sycl/integer_motion_sycl.cpp` ×1 | `misc-const-correctness` | Same shape as the two above |

The five `misc-const-correctness` comments all claimed clang-tidy "cannot see
the writes through SYCL `atomic_ref`". It can. `docs/state.md`
(`T-SYCL-LINT-SWEEP-2026-09-16`) had promoted that claim to "the measured
justification" for these markers; it is corrected there.

### Live but misdescribed — fixed properly

`sycl/integer_adm_sycl.cpp` `launch_dwt_hori_pair` carried
`NOLINTNEXTLINE(misc-unused-parameters)` reading "kept for ABI symmetry",
naming `buf_stride`. `buf_stride` **is** used (`auto e_buf_stride = buf_stride`).
The diagnostic is at the parameter `h_add`, which nothing in the body reads:
the horizontal pass derives its own rounding addend, `rnd = 1 << (h_shift - 1)`,
which equals every `h_add` the shift table supplies (`h_shift` 16 → 32768,
15 → 16384, all four rows). The CPU reference does the same — `add_shift_HP`
is a local constant beside `shift_HP` in `integer_adm.c`, not a parameter — so
this is not a parity surface either. The parameter, its `DwtShifts` field and
its call argument are removed; the TU rebuilds under `icpx` 2026.0 and no
arithmetic changes.

There is no "ABI symmetry" to preserve: the vertical sibling's `v_add` plays a
different role (`lo_val -= dwt_lo_sum * e_v_add`), not a rounding addend.

### Live and load-bearing — cited

| Sites | Check | Citation |
|---|---|---|
| `cuda/integer_adm_cuda.c`, `cuda/integer_vif_cuda.c` | `performance-no-int-to-ptr` (28 + 18 diagnostics) | ADR-0141 §2 — `CUdeviceptr` is an integer handle; the cast is the CUDA Driver API contract |
| 7 SYCL TUs | `misc-use-anonymous-namespace`, `misc-use-internal-linkage` | ADR-0141 §2 — entry-point addresses live in an `extern "C" VmafFeatureExtractor` struct, and a namespace cannot appear inside that linkage specification |
| `metal/ssimulacra2_metal.mm` | `readability-function-size` | ADR-0141 §2 — line-for-line scalar-diff parity port. Not verifiable here: Metal has no Linux toolchain, so this one is cited on the same grounds as its four already-cited siblings in the same file rather than on a measurement |
| `test_hip_float_vif_parity.c` | `readability-function-size` | [ADR-1264](../adr/1264-hip-scaffold-enosys-contract.md) — the `-ENOSYS` scaffold skip contract is checked after each of four HIP entry points |
| `test_sycl_motion3_parity.c` ×2 | `readability-function-size` | ADR-0141 §2 + ADR-0278 — same wording as the three already-cited siblings in the same file |

Tree-wide uncited after the sweep: **0**.

## Loose end

`count_uncited_nolints()`'s block-comment scan runs **forward only**, from the
marker to the closing `*/`. A citation written above the marker in the same
comment — the natural prose order, and what four of these sites had — does not
count. That is why several markers with honest ADR references still measured as
uncited. This sweep moved the tokens into the window rather than widening the
scan: loosening the counter would reduce the count without improving a single
comment, and the current rule ("the citation is adjacent to the marker") is
defensible on its own terms. Recorded here so the next reader does not mistake
it for a bug in the sweep.
