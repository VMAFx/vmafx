# Research-2074: HISS-21 replay-evidence gap

## Scope

Compare the governance claims shown on VMAFx `master` with the installed and
CI-pinned Praetor engines, then measure whether the claimed enforcement can be
replayed rather than inferred from a green audit.

## Findings

- The live README badge and canonical `AGENTS.md` ended at HISS-16. Current
  Praetor defines HISS-17 through HISS-21, including replayable enforcement
  evidence and platform neutrality.
- The manifest and lock `version: 1` fields are schema versions, not the HISS
  revision. They did not explain the stale HISS-16 label.
- `praetorctl hiss coverage --verify` failed on the unmodified tree with
  `no coverage catalog at .config/hiss/coverage.yaml`.
- The required standards workflow ran `compile-context --verify` and `audit`
  only. It could pass without executing the HISS-20 verifier.
- Praetor commit `846da5908d15b3cf5581ca6b0205cc644b249599`, already pinned by
  the workflow, contains the coverage verifier. No engine bump is required to
  close this gap.

## Evidence design

The catalog records only behavior the verifier replays itself. It covers six
scanner rules, 18 rule/language claims, and 43 fixtures across C, Go, Python,
and Rust. Each partial claim has at least one detecting fixture and, where the
scanner has a known boundary, a gap fixture. Legitimate negative fixtures guard
against overmatching.

Compiler warnings, ABI checks, supply-chain checks, context compilation, and
coverage floors are real VMAFx gates, but this verifier does not execute them.
Putting them in this catalog would prove attribution at most, not enforcement,
so they remain outside the replay claim.

## Required integration

The replay command must fail closed in four places: `make verify-all`, local
hooks, the required standards job, and a three-platform GitHub Actions matrix.
The required-check aggregator must name every matrix context and classify the
standards/replay contexts as strict-success checks. Naming alone is insufficient:
the generic aggregator policy accepts an absent check as a path-filter skip and
accepts `skipped` or `neutral` conclusions. A contract test therefore pins the
workflow matrix, local entrypoints, strict list, and success-only evaluator.

## Hosted validation follow-up

PR #1514 passed the replay job on Linux, macOS, and Windows. Its deliberate
draft failure also exposed an unrelated error cascade in the required Scorecard
workflow: `if: always()` tried to upload three artifacts after the ready-review
guard had stopped the job before any could exist. The upload now runs only for
non-draft attempts while retaining `if-no-files-found: error` for real scans.
The Scorecard PR workflow is also a full-impact authority path, so its contract
tests cannot be skipped by the impact planner when that workflow changes.

Guarded merge-train validation then exposed a separate fail-closed lint defect:
the installed Meson 1.12 completed setup and a 1,780-target build without
creating `core/build/compile_commands.json`. The configured-lint driver rejected
the missing input, correctly preventing a pass receipt, but every CI consumer
had made the same stale assumption that Meson would write the file. The fix is
an explicit, shared Ninja export of only `c_COMPILER` and `cpp_COMPILER`; its
contract tests reject missing rules, empty or malformed output, non-compilation
entries and failed Ninja calls while preserving the last valid database.

The next guarded run exposed the adjacent tool-resolution defect. Make had
exported an absolute venv path globally but each Meson recipe shadowed it with
relative `.venv/bin`. Meson resolved and persisted that relative Ninja name,
then tried to launch it from `core/build` during reconfiguration. The recipes
now prepend the venv's `abspath`, and the real Make fixture rejects any return
to a relative first `PATH` entry.

Once the analyzers could run, Clang 21 reported `log.c`'s initialized
`va_list` as uninitialized. Preprocessing showed why: C23 maps the standard
macro to `__builtin_c23_va_start`, which the VA-list analyzer does not model.
The Clang+C23 branch now uses the older, semantically identical modeled
builtin; GCC and MSVC retain standard `va_start`. The same pass renamed
`log.h`'s ISO-reserved double-underscore guard. Both C compilers build the
result warning-free and the log plus ORT-injection tests preserve behavior.

The first complete Cppcheck replay then separated analyzer-model defects from
source defects. Cppcheck 2.22 incorrectly marks `pthread_cond_init`'s attributes
pointer non-null, producing seven false `nullPointer` errors for the POSIX
default-attributes call; versions 2.13 through 2.21 omit the function model and
therefore miss a genuinely null condition object. The gate now derives a model
from the installed analyzer, inserts the correct contract when absent or
removes only the bad argument-2 marker when present, asks that same analyzer to
validate it, and publishes it atomically. Positive and negative controls cover
both pointers and the older install layout. No call-site suppression or warning
category changed.

That corrected replay exposed a real source defect in `vmaf_framesync_init`:
both mutex initializers and the condition initializer were unchecked. The
context is now published only after all three primitives and its first queue
node are ready; each failure returns the pthread error and destroys exactly the
already-initialized subset. A separately compiled test framesync object maps
only the four pthread init/destroy entry points to deterministic wrappers, so
all failure stages are covered without a production hook, linker-specific
interposition, executable-source include, or analyzer suppression. It also pins
the header's existing null-context destroy contract, whose implementation had
previously dereferenced the null pointer.

## Reproducer

```bash
praetorctl hiss coverage --verify
make hiss-coverage
python3 -m unittest discover -s scripts/ci/tests -p test_write_compile_commands.py
python3 -m unittest discover -s scripts/ci/tests -p test_lint_configured.py
python3 -m unittest discover -s scripts/ci/tests -p test_cppcheck_posix_model.py
meson test -C core/build test_framesync test_framesync_init_failure --print-errorlogs
```

Deleting the catalog, replacing a positive fixture with clean code, or turning
a negative fixture into a matching violation must make both commands nonzero.
