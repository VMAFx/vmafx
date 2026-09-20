# Research-2067: Exact lexical measurement of NOLINT citations

Measurements behind [ADR-1266](../adr/1266-gpu-nolint-citation-closeout-round-2.md).
Historical numbers use Git blobs from `origin/master` at `8d0cdd7c4`; they do
not accidentally open the working-tree version of the same path.

## Policy under test

[ADR-0141](../adr/0141-touched-file-cleanup-rule.md) requires each `NOLINT`
to cite the ADR, research digest, or rebase invariant that forces it. The
citation has to belong to the marker's own explanation; physical proximity to
an unrelated comment is not ownership.

The shipped counter mixed three rules:

1. a previous/same/next physical-line window;
2. a forward-only scan of a `/* ... */` block; and
3. no model at all for a multi-line `//` explanation.

The replacement lexes comments while skipping ordinary strings, character
literals, and C++ raw strings. It treats one block or one maximal contiguous
`//` run as the exact comment unit and searches only that unit for `ADR-NNNN`.

## Historical measurement

| measurement | markers | paths |
| --- | ---: | ---: |
| shipped scanner | 46 | 25 |
| false reports among those 46 | 16 | 16 |
| genuinely uncited among those 46 | 30 | 9 |
| violations hidden by the ±1-line shortcut | 33 | 23 |
| exact lexical scanner | 63 | 32 |

The arithmetic is `46 - 16 + 33 = 63`: the old defects partially cancelled.

The shipped 46 were distributed as follows:

| subtree | markers |
| --- | ---: |
| `core/src/feature/sycl/` | 37 |
| `core/src/feature/cuda/` | 3 |
| `core/src/feature/metal/` | 1 |
| `core/src/sycl/` | 2 |
| `core/test/` | 3 |

### Sixteen false reports

All sixteen already contained a citation in the same explanatory comment:

- two SSIMULACRA2 block comments cite ADR-0141 before the marker;
- two CUDA contiguous `//` runs cite ADR-0278 before the marker; and
- twelve SYCL contiguous `//` runs cite ADR-0278 after the marker.

The forward-only block scan missed the first shape. Treating each `//` line as
unrelated missed the other two shapes.

### Thirty-three hidden violations

The physical-line shortcut credited 33 markers whose own comment had no ADR.
They span 23 paths under `core/src/feature/`, `core/src/log.cpp`,
`core/src/thread_pool.c`, `core/test/`, and `core/tools/yuv_input.c`.
An ADR in a separate comment immediately before or after the marker is not an
inline explanation, even when the two comments are adjacent. One HIP marker
was diagnostics-free and was deleted; the other 32 now carry the citation in
the marker's exact comment.

Tests pin the boundaries that exposed this class: adjacent block comments in
both orders, two blocks on one physical line, a blank or code line ending a
`//` run, ADR-looking code and literals outside a comment, fake comment tokens
inside strings, and multiple markers in one comment.

## Refactor-first audit of the original thirty

Initial classification by marker text was:

| class | count |
| --- | ---: |
| C-linkage / internal-linkage brackets | 7 |
| `misc-const-correctness` | 15 |
| `readability-isolate-declaration` | 2 |
| `bugprone-branch-clone` | 2 |
| `misc-unused-parameters` | 1 |
| parity-test `readability-function-size` | 3 |

Reading and probing the target code changed the outcome:

| outcome | count | evidence |
| --- | ---: | --- |
| anonymous-namespace refactor | 7 | callback types are declared outside the public header's C-linkage block; only the exported descriptors need explicit C linkage |
| semantics-neutral kernel fixes | 8 | two const pointer bindings, two split declarations, two branch simplifications, one unused parameter removed, one `std::find` lookup |
| diagnostics-free kernel markers deleted | 10 | comment-stripped probes through the repository SYCL wrapper with LLVM 20.1.8, 21.1.8, and 22.1.8 |
| cleanup-safe parity-test refactors | 3 | shared frame-feed, score-read, scaffold-skip, and context-cleanup helpers replace function-size suppressions |
| retained template suppressions | 2 | scale 3 folds `DO_RD` writes away and triggers `misc-const-correctness`; scales 0-2 instantiate and mutate the same two accumulators |

The ten dead markers were on direct `+=` reductions or convolution
accumulators. Removing them produces no target diagnostic. Only
`h_ref_rd` and `h_dis_rd` in `integer_vif_sycl.cpp` reproduce across all three
LLVM versions, twice each for the scale-3 SG16 and SG32 instantiations.

## Separate dead-marker audit

The scan review exposed another fragile pattern: a `NOLINTNEXTLINE` on the
first line of a multi-line comment suppresses the comment's second line, not
the declaration below the whole comment. A broader target-level audit therefore
checked whether each marker suppressed any matching diagnostic rather than
merely moving it.

Beyond the 63 historical violations, four already-cited no-op code markers
were deleted from GPU dispatch, logging, and the AVX2 / AVX-512 ADM helpers.
Two duplicated function-size suppression pairs in
`test_sycl_motion_add_uv_parity.c` were removed by sharing cleanup-safe test
helpers. The HIP marker deleted from the hidden 33 is not counted a second
time.

## Result and limits

- Current exact tree-wide count: **0 markers across 0 paths**.
- Focused scanner suite: **27 passed**, including 18 tests in
  `UncitedNolints`.
- SYCL and HIP builds link successfully after the namespace and kernel
  refactors.
- Focused backend test executables pass their registration paths. This host has
  no usable SYCL device, and HIP float VIF reports its documented `-ENOSYS`
  scaffold skip, so those runs are not device-parity evidence.
- The committed SYCL ratchet baseline's measured-source list omits the feature
  kernel TUs. The changed-file SYCL wrapper does cover them, but the two
  retained template suppressions are not represented in the lane baseline.

Because the scanner itself changed, every tidy lane needs a full
remeasurement. A scoped write can verify a file, but it cannot establish a
new global scanner contract.

## Reproducing

The historical comparison loads both implementations, enumerates paths from
the base commit, and reads every source as a blob with `git show
8d0cdd7c4:<path>`. The resulting totals are:

```text
shipped 46 25
exact   63 32
```

Current-tree checks:

```sh
python3 -m pytest scripts/ci/tests/test_tidy_ratchet.py -q
python3 scripts/ci/gen-sycl-compile-commands.py build-sycl
CLANG_TIDY_BIN=clang-tidy scripts/ci/clang-tidy-sycl.sh \
  -p build-sycl --checks=-*,misc-const-correctness --quiet \
  core/src/feature/sycl/integer_vif_sycl.cpp
```
