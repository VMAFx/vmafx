# NOLINT Cluster Audit — 2026-05-29

<!-- markdownlint-disable MD013 -- ADR/research body text; pre-existing long lines per ADR-0864 tail -->

**Scope**: all `NOLINT` / `NOLINTNEXTLINE` / `NOLINTBEGIN`…`NOLINTEND` annotations
under `core/src/` with five or more occurrences of the same suppression category in a
single file or closely related group of files.

**Total annotations found**: 218 across 62 files.

---

## Cluster 1 — SYCL `misc-const-correctness` (atomic_ref variables)

**Files**: `core/src/feature/sycl/integer_vif_sycl.cpp` (8),
`core/src/feature/sycl/integer_adm_sycl.cpp` (6)

**Root cause**: clang-tidy's `misc-const-correctness` check cannot trace writes
through `sycl::atomic_ref<>` or sub-group reduction intrinsics. Variables that
accumulate via atomic operations are flagged as "could be const" even though they are
mutated at runtime. This is a well-known analyser limitation (the analyser sees the
variable type, not the aliased atomic write path).

**Refactor candidate**: a single file-scope comment block at the top of each SYCL TU
covered by an existing `NOLINTBEGIN(misc-const-correctness)` … `NOLINTEND` pair would
collapse all inline suppressions into one guarded region, matching the existing
`misc-use-anonymous-namespace` / `misc-use-internal-linkage` pattern already in use at
the top and bottom of both files. This would reduce 14 inline annotations to 0 (folded
into the block already present) with no semantic change.

**Justifiable?** Yes — but could be tighter. The existing BEGIN/END block at the file
boundary could be extended to also cover `misc-const-correctness`. The inline
suppressions are redundant given the file-level block approach.

**Recommendation**: extend the `NOLINTBEGIN` / `NOLINTEND` blocks in both SYCL TUs
to include `misc-const-correctness`. Remove the 14 inline annotations. ADR-0278
(NOLINT citation closeout) already sanctions this category for SYCL.

---

## Cluster 2 — SYCL `bugprone-implicit-widening-of-multiplication-result` (stride arithmetic)

**Files**: `core/src/feature/sycl/integer_adm_sycl.cpp` (8),
`core/src/feature/sycl/integer_vif_sycl.cpp` (4)

**Root cause**: SYCL global-id / stride index computations multiply two
`int`-width operands; clang-tidy warns that the product could overflow before
widening. These are kernel `nd_range` bounds that are architecturally bounded,
so the widening is the correct intent, not a bug.

**Refactor candidate**: wrap each multiplication as `(ptrdiff_t)(a) * (b)` to make
the widening explicit and satisfy the linter without a suppression. This is a
mechanical one-line fix per site — no semantic change, no performance impact, and it
eliminates the need for the suppression entirely. 12 annotations across two files
could be deleted.

**Justifiable?** Suppression is defensible but unnecessary — a cast is cleaner and
more portable to future analysers.

**Recommendation**: replace every `(size_t)a * b`-style SYCL stride expression with
`(ptrdiff_t)(a) * (ptrdiff_t)(b)` or equivalent explicit-width cast. Remove the
12 suppressions. No ADR needed (straightforward cast fix).

---

## Cluster 3 — SYCL `readability-function-size` (kernel entry points)

**Files**: `integer_adm_sycl.cpp` (6), `integer_vif_sycl.cpp` (5)

**Root cause**: SYCL kernel-launch entry points are structurally large because they
must declare all accessors and capture them in a single `parallel_for` lambda. Any
split would either require a helper free function that the compiler cannot inline back
into device code, or a macro expansion that trades line-count for readability.
This is the pattern documented in ADR-0141 §2 as a load-bearing invariant.

**Refactor candidate**: none viable. The existing citations (ADR-0141, ADR-0278) are
correct. These 11 suppressions are justified.

**Recommendation**: no change needed. Suppressions are load-bearing.

---

## Cluster 4 — `performance-no-int-to-ptr` in slab allocators

**Files**: `core/src/feature/hip/integer_vif_hip.c` (16),
`core/src/feature/cuda/integer_vif_cuda.c` (3),
`core/src/feature/cuda/integer_adm_cuda.c` (2)

**Root cause**: GPU backends use a single contiguous `malloc` slab partitioned by
bumping a `uint8_t *ptr` pointer, then casting sub-ranges to typed pointers
(`uint16_t *`, `uint32_t *`, etc.). This is the standard arena/slab allocator
pattern; `performance-no-int-to-ptr` fires because the cast goes through pointer
arithmetic, not from an integer value.

**Problem**: these suppressions carry **no ADR citations** — they are bare
`/* NOLINTNEXTLINE(performance-no-int-to-ptr) */` with no justification comment. This
violates ADR-0278 (every NOLINT must cite an ADR / research digest / rebase
invariant inline).

**Refactor candidate A (preferred)**: add a named helper macro or inline function
`SLAB_FIELD(ptr, type)` that wraps the cast and moves the suppression to one
definition site. This reduces 16+3+2 = 21 inline annotations to a single guarded
definition. The macro pattern is already used in `core/src/feature/integer_adm.c`
for `bugprone-macro-parentheses` (lines 262–747 block).

**Refactor candidate B (minimal)**: add inline citations to the existing
suppressions, referencing the slab-allocation pattern and an appropriate ADR
(e.g. ADR-0278 itself, or a new ADR if the slab pattern needs its own entry).

**Justifiable?** Yes — slab allocation is a legitimate use of pointer arithmetic,
but the missing citations make it non-compliant with ADR-0278.

**Recommendation**: implement the `SLAB_FIELD` macro in a shared GPU helper header
(e.g. `core/src/feature/gpu_slab.h`) and migrate all three files. This eliminates
21 bare NOLINTs and establishes a reusable pattern for future GPU backend additions
(HIP, Vulkan).

---

## Cluster 5 — `readability-function-size` in `integer_adm.c` (CPU scalar)

**File**: `core/src/feature/integer_adm.c` (13 bare annotations, lines 752–3580)

**Root cause**: ADM has intrinsically large band-processing functions (one per
scale × decomposition pass). Each function is a tight numerical loop with a fixed
structure; splitting would require passing 15–20 local variables or packaging them
in a struct, adding indirection that the compiler cannot always eliminate.

**Problem**: all 13 suppressions are **bare** (`// NOLINTNEXTLINE(readability-function-size)`)
with no ADR citations. ADR-0278 non-compliant.

**Refactor candidate**: the NOLINTBEGIN/NOLINTEND block already present at lines
262–747 for `bugprone-macro-parentheses` + `bugprone-implicit-widening` demonstrates
that block-form suppression is acceptable here. Extending the block (or adding a
second block) to cover `readability-function-size` for the band-processing functions
would consolidate 13 individual annotations and require only one citation comment.

**Recommendation**: add a `NOLINTBEGIN(readability-function-size)` /
`NOLINTEND(readability-function-size)` block around the band-processing section with
a single citation comment (ADR-0141 §2 / ADR-0278), replacing 13 individual bare
annotations. Alternatively, add inline citations to each existing annotation.

---

## Summary table

| Cluster | File(s) | Count | Root cause | Refactorable? | Priority |
| --- | --- | --- | --- | --- | --- |
| SYCL misc-const-correctness | `sycl/integer_{vif,adm}_sycl.cpp` | 14 | Analyser blind to atomic_ref writes | Extend NOLINTBEGIN block | Medium |
| SYCL bugprone-implicit-widening | `sycl/integer_{adm,vif}_sycl.cpp` | 12 | Stride mult without explicit cast | Replace with explicit cast (remove NOLINT) | High |
| SYCL readability-function-size | `sycl/integer_{adm,vif}_sycl.cpp` | 11 | SYCL kernel-launch pattern (load-bearing, ADR-0141) | No — justified | None |
| GPU slab `performance-no-int-to-ptr` | `hip/integer_vif_hip.c`, `cuda/*` | 21 | Slab allocator, missing citations | `SLAB_FIELD` macro in shared header | High |
| CPU ADM `readability-function-size` | `integer_adm.c` | 13 | Band-processing functions, missing citations | Extend NOLINTBEGIN block | Medium |

**Total refactorable**: 47 of 71 clustered annotations (66%). The remaining 24
(SYCL readability-function-size) are load-bearing and correctly cited.

---

## Recommended implementation sequence

1. **PR A**: Add explicit `(ptrdiff_t)` casts to SYCL stride arithmetic in
   `integer_adm_sycl.cpp` and `integer_vif_sycl.cpp` — removes 12 NOLINTs
   without any semantic change.
2. **PR B**: Introduce `core/src/feature/gpu_slab.h` with `SLAB_FIELD` macro;
   migrate `hip/integer_vif_hip.c`, `cuda/integer_vif_cuda.c`,
   `cuda/integer_adm_cuda.c` — removes 21 bare NOLINTs, adds one cited definition.
3. **PR C**: Extend SYCL `NOLINTBEGIN` blocks to cover `misc-const-correctness`;
   extend or add `NOLINTBEGIN(readability-function-size)` in `integer_adm.c` with
   ADR citations — removes 14+13 = 27 inline annotations.

PRs A–C are independent (different files) and can be staged in parallel worktrees.
