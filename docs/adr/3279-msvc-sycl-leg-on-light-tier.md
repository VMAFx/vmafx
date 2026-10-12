<!-- markdownlint-disable MD013 MD060 -->

# ADR-3279: One MSVC build leg runs on light-tier pull requests that change core C or C++

- **Status**: Accepted
- **Date**: 2026-10-12
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `testing`

## Context

Pull request #2724 changed C and C++ under `core/src` and merged with every required
check green. The first master run after it was red on every MSVC job: the new code
called `std::min` and `std::max` in a header that the Windows SDK's `min` and `max`
macros rewrite. The pull request's own checks could not show it. Its tier was light
([ADR-2169](2169-ci-fewer-runs.md)), and the light tier ran no Windows compile of the
core:

- `Windows MSVC+CUDA`, `Windows MSVC+SYCL` and `Windows ARM64 MSVC` are full-tier
  contexts, so the check list of the pull request did not contain the first two and
  showed the third as skipped.
- The MinGW lane (`Windows UCRT64`) is a full-tier context too, and it would not have
  shown the defect: mingw-w64 defines the two macros only for C, and the lane passed
  on the master runs the MSVC jobs failed on.

Both MSVC jobs failed on the three master heads that were measured (`84867ebbc`,
`768cf5b0c`, `c13628fdc`) until #2740 fixed the code. That pull request also
added a source scanner for this one pattern (`scripts/dev/find-windows-minmax.py`, stage
`msvcism` of `make preflight`). The scanner covers one cause; the class is wider: any
change to core C or C++ that only the MSVC front end or the Windows SDK headers refuse.

Measured on the `Builds` run of the fixed master commit (run 38153818144, 2026-10-11):

| Job | Result | Duration |
| --- | --- | --- |
| `Windows MSVC+SYCL` | success | 32.1 min |
| `Windows MSVC+CUDA` | success | 59.4 min, against a job timeout of 60 min |
| `Windows ARM64 MSVC` | failure (a test timeout, not a compile error) | 25.5 min |
| `Windows UCRT64` | success | 26.2 min |

A second defect showed while this was measured. The label `ci: full` starts
`ci-escalate.yml`, which cancels each run that is still in progress, waits for it and
re-runs it. In run 38164022446 (2026-10-11) the script re-ran six workflows, then ended
with `HTTP Error 409: Conflict`, and no workflow after the sixth was re-run. The log does
not name the request that was refused. GitHub's REST reference lists 409 as an answer of
"Cancel a workflow run" and not of "Re-run a workflow", so the cancel of a run that had
completed between the listing and the cancel is the likely source; that is inferred from
the reference, not observed.

## Decision

1. **`Windows MSVC+SYCL` becomes an own-input lane.** It stays in `full_only` of
   `.github/ci-tier.json` and gets an entry in `own_input_lanes`, as `Windows Tester Zip`
   has ([ADR-2198](2198-windows-sycl-leg-and-cut-check.md)). A light-tier pull request
   runs the leg when its diff selects `windows_msvc_sycl`. `Windows MSVC+CUDA` and
   `Windows ARM64 MSVC` stay full-tier.
2. **Selector `windows_msvc_sycl`** in `.github/ci-impact.json`, own paths only
   ([ADR-1700](1700-tester-selectors-own-paths-only.md)): C, C++ and header files under
   `core/` (`*.c`, `*.h`, `*.cpp`, `*.inc`, `*.h.in`), the Meson files under `core/`,
   and the leg's own inputs (`libvmaf-build-matrix.yml`, `scripts/ci/werror-args.sh`,
   `scripts/ci/check_msvc_library_names.py`, `scripts/ci/run_meson_test.py`,
   `requirements/locks/build.txt`).
3. **A planner job in the workflow.** `libvmaf-build-matrix.yml` gets the job `impact`
   (`Plan MSVC leg impact`), which runs `scripts/ci/plan-ci-impact.py` on every tier
   that runs the workflow. `windows-gpu-build` starts when the tier is full, or the
   planner selected the leg, or the planner failed. The last term fails closed: a
   planner that cannot answer costs one build, never a missed one. The job does not
   start in a cancelled run.
4. **Each matrix row names its tier.** A matrix cannot be filtered per row, so both rows
   start when the job starts. The first step reads the row's `tier` key and the tier
   decision and writes `run`. The checkout after it is unconditional and shallow, because
   the checkout-ordering gate counts no checkout that carries an `if`; every step after
   the checkout is gated on `run`. On a selected light-tier pull request the SYCL row
   builds, checks the installed library names and runs the two SYCL registration tests
   (`test_sycl_kernel_registration`, `test_sycl_coff_anchor`); the CUDA row prints the
   ADR-2169 notice and ends without a build. Its check then shows success: read the notice, not
   the colour, to know whether MSVC+CUDA compiled.
5. **The escalation survives a refused request.** `scripts/ci/ci_escalate.py` treats 409
   on the cancel as "already completed" and awaits and re-runs the run as before. A run
   that cannot be re-run for any reason is reported with the reason, the runs after it
   are still started, and the step then exits 1 with the count.

The decision is reversible CI policy. It is looked at again when a break shows up that
only the CUDA or only the ARM64 MSVC leg would have caught.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| All three MSVC legs on light-tier pull requests | Catches CUDA-only and ARM64-only MSVC breaks too | 117 min of Windows runner time per core pull request by the measurement above; the CUDA leg runs within a minute of its timeout | The measured failure class needs one MSVC compile of the shared core, not three |
| `Windows MSVC+CUDA` in place of the SYCL leg | Also compiles the CUDA host code | 59.4 min against 32.1 min, and a 60-minute timeout | The slower leg for the same class |
| Rely on the min/max scanner of #2740 | No runner time | Covers one pattern; does not follow SDK headers or macro bodies | A scanner for one cause is no compile |
| Build the matrix from the planner's output (`fromJSON`) | Only the selected row would start | The row list leaves the workflow file, where the `-Werror` contract and the checkout-ordering gate read it | Two gates would lose their source of truth |
| A matrix `exclude` computed from the tier | No start-and-stop row | `exclude` takes no expression per row that reads another job's output | Not expressible |
| A second job that holds only the SYCL row | No start-and-stop row | The 16 build and test steps would exist twice | One behaviour, one implementation (HISS-19) |
| A separate gate job in front of the leg, as the tester zip has | Same shape as ADR-2198 | Adds a required context and a job for a lane whose context is the leg itself | The leg's own name is already the context |

## Consequences

- **Positive**: a pull request that changes core C, C++ or Meson files compiles with MSVC
  before it lands, in about 32 minutes beside the other light-tier jobs. A pull request
  that changes none of the selector's paths starts no Windows GPU runner. The label
  `ci: full` now re-runs every workflow it can and names each one it could not.
- **Negative**: every such pull request also starts the CUDA row, which ends after a
  shallow checkout with a notice and a green check. The planner job adds one short Linux
  job to every run of the workflow.
- **Neutral / follow-ups**:
  - Outside the selector, so still caught only on the master push or with `ci: full`:
    NASM sources (`*.asm`), the Python build helpers under `core/`, `core/api/vmafx.toml`,
    symbol lists (`*.def`, `*.map`), Rust sources (off by default) and CUDA device
    sources (`*.cu`, `*.cuh`).
  - The aggregator's known limit is unchanged: a leg that registers only after the wait
    loop ended is not seen. The tester-zip lane has the same limit.
  - `scripts/ci/tests/test_ci_routing_contract.py` plants ten defects against this lane
    (the lane or its planner back behind the full tier, a row with the wrong tier, a step
    that ignores the row's decision, a checkout behind that decision or with the full
    history, a job that skips on a failed planner or starts in a cancelled run), and
    `scripts/ci/tests/test_ci_escalate.py` fails on the script as it was before this
    change (six tests).
  - The hosted evidence for the leg on a light-tier pull request is the pull request
    that carries this ADR: it changes the workflow file, which the selector owns.

## References

- Decision `vmafx-routing-gap-windows-leg`, 2026-10-11, on which MSVC legs a light-tier
  pull request runs (paraphrased): one MSVC leg, `Windows MSVC+SYCL`, on light-tier pull
  requests that touch core C or C++; the CUDA and ARM64 MSVC legs stay full-tier; the
  record states the measurement and a re-look when a break shows up that only one of
  those two legs would catch.
- Pull request #2724 (the change that merged green), pull request #2740 (the fix and the
  scanner), runs 38153818144 (durations) and 38164022446 (the escalation that stopped).
- GitHub REST API reference, "Cancel a workflow run" and "Re-run a workflow", read
  2026-10-12.
- [ADR-2169](2169-ci-fewer-runs.md): the tiers and the escalation this record amends.
- [ADR-2198](2198-windows-sycl-leg-and-cut-check.md): the first own-input lane.
- [ADR-1700](1700-tester-selectors-own-paths-only.md): selectors that answer for their
  own paths only.
- [ADR-1140](1140-ci-impact-planner.md): the impact planner.
- [ADR-0121](0121-windows-gpu-build-only-legs.md): the two Windows GPU legs.
