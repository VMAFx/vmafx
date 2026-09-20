# Research-2068: Whole-tree zero-debt inventory

Evidence captured when ADR-1267 replaced changed-file and permanent-ratchet
acceptance with a mandatory zero endpoint. Counts are inventories, not
allowances.

## Pinned measurements

Praetor was run from the repository root at exact engine commit
`846da5908d15b3cf5581ca6b0205cc644b249599`. The first measurement after the
NOLINT closeout's partial refactors found **1,378 HISS infractions across 517
files** in a supported-source scope of 1,974 files:

| Rule | Findings |
|---|---:|
| HISS-01 control flow / recursion | 391 |
| HISS-02 unbounded loops | 64 |
| HISS-04 function size | 799 |
| HISS-07 unchecked Rust errors | 74 |
| HISS-08 banned unsafe libc | 2 |
| HISS-09 missing safety proofs | 48 |

The largest top-level queues were `core/` 823, `cmd/` 119, `ai/` 103,
`pkg/` 88, `python/` 71, `tools/` 65, and `compat/` 54. The inventory includes
Netflix-mirror, vendored, GPU, SIMD, tests, and tools; none is excluded because
of origin.

The four committed clang-tidy lane inventories contained **4,288
lane-counted diagnostics**:

| Lane | Diagnostics | Files with diagnostics |
|---|---:|---:|
| CPU | 775 | 132 |
| CUDA | 1,449 | 173 |
| HIP | 1,249 | 147 |
| SYCL | 815 | 125 |

The lane sum is not a unique-finding count: common headers and CPU translation
units can appear in more than one build profile. It is the exact work queue
each lane must independently reduce to zero. The corrected exact-comment
NOLINT scanner separately reports zero uncited markers.

The assertion-density gate had two independent scope defects: copyright
selection excluded Netflix and other origins, and its function detector
classified `static`, `extern`, `inline`, `const`, `struct`, and `enum` as if
they were control-flow keywords. Removing both blind spots scans 730 tracked
C-family source files and reports **1,473 non-trivial assertion-free functions
across 423 files**. The scanner recognizes 4,916 functions in total and counts
346 assertions. The queue is concentrated in `core/` (1,465 findings), with
six compatibility-MEX and two command findings. Nine hermetic regression cases
pin both file selection and enforcement of long static, Netflix-authored, and
headerless functions.

## Why the old green result was insufficient

`standardsctl audit` without a changed-file base passed whenever current HISS
fingerprints stayed within `.standards-baseline.json`. The tidy ratchet likewise
passed when measurement equalled `tidy-baseline-<lane>.json`. Those results
proved no unrecorded growth, not HISS-10 compliance. Changed-file jobs also
excluded toolchain-specific families and explicitly described some vendored
mirrors as untidyable.

The inventories remain useful for deterministic partitioning and before/after
receipts. They are not evidence that a repository with nonzero entries is
clean.

## Compiler-warning root causes found during the sweep

The Intel oneAPI 2026.0 SYCL compile commands exposed two build-policy bugs,
not harmless tool noise. The unqualified `-Xs '-device ...'` argument was
unused when paired with the `spir64_gen,spir64` multi-target compile, and the
compile-only custom targets omitted `-fno-sycl-rdc`; the intended native AOT
images were therefore not materialized in their fat objects. Scoping the
option with `-Xsycl-target-backend=spir64_gen` and disabling relocatable device
code makes the driver invoke `ocloc` while retaining the `spir64` fallback.

Three scalar-reference libraries also passed both `-fp-model=precise` and
`-ffp-contract=off` to icx. Intel documents `-fp-model=strict` as value-safe
with contraction disabled, so one strict-model flag preserves the intended
arithmetic contract without the compiler's overriding-option warning. GCC and
ordinary Clang retain their existing `-ffp-contract=off` spelling.

The Python inventory also exposed **253 `E402` findings under `ai/`**. These
were not isolated import-style mistakes: ADR-0681's direct-script bootstrap
required each entrypoint to execute a path-mutation call before importing
repository packages, and the resulting violations were hidden with per-line
`noqa` comments. ADR-1268 moves the fixed, repository-owned root installation
to the private helper's own import and leaves script-specific metadata
resolution after the static import block. This preserves documented direct
invocation without replacing static imports with analyzer-opaque dynamic
loading.

## Cleanup contract

Each wave must:

1. select disjoint paths from the full inventory, never only the diff;
2. remove findings structurally rather than add blanket suppressions;
3. preserve arithmetic order and golden assertions in numerical code;
4. run the most specific build and parity/unit tests for the paths;
5. rerun the exact pinned whole-tree audit and applicable tidy lanes;
6. mechanically re-record lower inventories, never hand-edit them; and
7. continue into the next queue until every rule and lane is zero.

Repository completion requires all of the following in the same final state:

```text
Praetor HISS total                   0
clang-tidy CPU/CUDA/HIP/SYCL        0 / 0 / 0 / 0
all NOLINT markers                  0
assertion-density findings          0
cppcheck errors/warnings            0
compiler warnings/errors            0
make lint / make test / verify-all  pass
Netflix golden assertion edits      0
```

An unavailable backend is an explicit missing receipt, not a zero. Generated
code is repaired at the generator, and a vendored-source repair records the
delta that must be preserved at the next re-vendor.

## Reproduction

```sh
go run github.com/cordanaLLM/praetor/cmd/standardsctl@846da5908d15b3cf5581ca6b0205cc644b249599 audit
make tidy-ratchet LANE=cpu TIDY_RATCHET_BUILD_DIR=build
make tidy-ratchet LANE=cuda TIDY_RATCHET_BUILD_DIR=build-cuda
make tidy-ratchet LANE=hip TIDY_RATCHET_BUILD_DIR=build-hip
make tidy-ratchet LANE=sycl TIDY_RATCHET_BUILD_DIR=build-sycl
bash scripts/ci/assertion-density.sh
```

The baseline JSON files expose per-file queues and must end with zero totals;
full command output is retained as run-scoped evidence during each wave.
