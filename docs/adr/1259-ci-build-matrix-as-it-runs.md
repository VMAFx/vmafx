<!-- markdownlint-disable MD060 -->
# ADR-1259: Record the CI build matrix as it actually runs

- **Status**: Accepted, Supersedes
  [ADR-0689](0689-vmafx-ci-matrix-dedupe.md),
  [ADR-0691](0691-vmafx-drop-legacy-build-paths.md),
  [ADR-0710](0710-vmafx-ci-slim-down-v2.md),
  [ADR-0728](0728-native-build-sunset.md)
- **Date**: 2026-09-18
- **Deciders**: Lusoris
- **Tags**: ci, build, fork-local

## Context

Four Accepted ADRs from 2026-05-28 describe a smaller build matrix than the
one CI runs. Traced to their commits:

- ADR-0689 (`0ab991afc`, #1567) removed five rows from
  `.github/workflows/libvmaf-build-matrix.yml`: `Build — Ubuntu gcc (CPU)`,
  `Build — Ubuntu clang (CPU)`, `Build — macOS clang (CPU)`, the MoltenVK row
  (moved to `nightly.yml`) and the dynamic `Build — Ubuntu CUDA` row.
- ADR-0691 (`9aa008e70`, #1564) removed `Build — Windows MinGW64 (CPU)` from
  the matrix and from the aggregator's required list, and removed
  `Build — Ubuntu i686 gcc (CPU, no-asm)`.
- About an hour later, `384d97d03`, the squash-merge of the `libvmaf/` →
  `core/` rename that "merge-resolves 42 master commits", put all of that back:
  the ADR-0689 rows, both ADR-0691 lanes and the MinGW64 aggregator entry. It
  also deleted the `nightly.yml` MoltenVK job. No ADR asked for any of it.
- ADR-0710 (`4e211736f`, #23) says `build.yml` "replaces
  `libvmaf-build-matrix.yml`", drops 18 jobs including `Cppcheck` and the
  three per-PR sanitizer rows, and updates the aggregator. The commit added
  `build.yml` and `sanitizers.yml` and changed no other workflow, so it
  dropped none of the 18 jobs, and both build workflows have run on every PR
  since.
- ADR-0728 (`bfd4c436b`, #52) calls itself the mechanical execution of
  ADR-0691 and ADR-0710: 14 lane removals including the Windows SYCL leg,
  `Cppcheck` and the per-PR sanitizers out of the aggregator, and the
  `build.yml` and `sanitizers.yml` names in. Its commit message and PR body
  describe those workflow edits, but the commit changed three files: the ADR,
  `changelog.d/removed/native-build-sunset.md` and
  `docs/development/deprecations.md`. ADR-0728 was never in force. The lanes
  it names that are gone today went for other reasons: the Vulkan and MoltenVK
  rows with the Vulkan backend (ADR-0726), and the i686 lane with ADR-1258.

Later ADRs were written against the matrix that actually ran. ADR-1234 gives
preflight stages that mirror `Ubuntu gcc(+DNN)`, `Ubuntu clang(+DNN)`, the two
`Windows MSVC` lanes, the sanitizers and `Cppcheck`, and cites failures on
`Windows MinGW64`. ADR-1253 and ADR-1254 fix defects found on the required
`Windows MinGW64` lane. ADR-1225 moves the required `Ubuntu HIP` lane to
ROCm 10. ADR-1245 and ADR-1246 extend the required `Cppcheck` job. Meanwhile
the ADR index showed the four ADRs as Accepted, and
`docs/development/deprecations.md` told contributors that MinGW64 was gone
while it was a required check.

## Decision

This ADR is the record of the CI build matrix: the lanes in the tables below
run as described. The maintainer settled three open points:

1. **32-bit x86**: the fork stays 64-bit only.
   [ADR-1258](1258-keep-64-bit-only-retire-i686-lane.md) carries this and
   removes the i686 lane and preflight's `m32` stage.
2. **Lanes with no later ADR**: `Ubuntu SYCL`, `Ubuntu SYCL+CUDA`,
   `Ubuntu gcc static`, `Ubuntu CUDA static` and `macOS clang+DNN` stay as
   they are, and stay not required.
3. **Required checks**: `Cppcheck` and the three
   `Sanitizers (address|thread|undefined)` jobs stay required. The
   `sanitizers.yml` jobs stay not required.

ADR-0689, ADR-0691, ADR-0710 and ADR-0728 are superseded. What still holds of
them is restated here: `build.yml` runs its three rows and `sanitizers.yml` its
three jobs (from ADR-0710), and ADR-0691's 64-bit-only rule is now ADR-1258's.

Decision 2 was asked about the five lanes ADR-0728 names. Five more lanes are
in the same position because ADR-0689 or ADR-0710 removed them only on paper:
`Ubuntu gcc`, `Ubuntu clang`, `macOS clang`, `Ubuntu ARM clang` and
`Ubuntu CUDA`. This ADR records them as they run and makes no new decision
about them. Removing any lane, or changing what is required, needs its own ADR
and the matching change to `required-aggregator.yml`.

In the tables, **required** means the name is in the `required` list of
`.github/workflows/required-aggregator.yml`, which is the only context branch
protection enforces. **Not required** means the lane reports, but a failure
does not block a merge. **continue-on-error** marks the `experimental: true`
rows, whose failure does not even fail the workflow run.

### `libvmaf-build-matrix.yml` (workflow `Builds`)

Every lane runs on non-draft PRs and master pushes. The 14 rows of the
`libvmaf-build` job first ask the CI impact planner (ADR-1140) and report
success without building when a change does not touch the C core; the three
Windows lanes build every time.

| Lane | What it runs | Status | Owning record | Later ADRs, notes |
| --- | --- | --- | --- | --- |
| `Ubuntu gcc` | CPU build, meson tests, tox | not required | upstream Netflix matrix, folded in by ADR-0115 | removed by ADR-0689, restored by `384d97d03`; mirrored by ADR-1234's `gcc` stage |
| `Ubuntu clang` | CPU build, meson tests, tox | not required | upstream, ADR-0115 | removed by ADR-0689, restored by `384d97d03`; ADR-1234's `clang` stage |
| `macOS clang` | CPU build, meson tests, tox | not required, continue-on-error | upstream, ADR-0115 | removed by ADR-0689, restored by `384d97d03` |
| `Ubuntu ARM clang` | CPU build, meson tests, tox on `ubuntu-24.04-arm` | not required | upstream (`b53406e36`) | dropped by ADR-0710 on paper only; ADR-1052 |
| `Ubuntu gcc+DNN` | CPU and ONNX Runtime build, meson dnn suite, meson tests, tox | required, also in `mustReport` | ADR-0120 | ADR-1234 |
| `Ubuntu clang+DNN` | as `Ubuntu gcc+DNN`, with clang | required | ADR-0120 | ADR-1234 |
| `macOS clang+DNN` | as `Ubuntu gcc+DNN`, on macOS | not required, continue-on-error | ADR-0120 | decision 2 |
| `Ubuntu HIP` | HIP host build against ROCm 10 without device kernels, HIP smoke test | required | ADR-0212 | ADR-1225, ADR-1234; ADR-1204 (Proposed) |
| `macOS Metal` | Metal build, meson tests including the Metal smoke test, tox | not required | ADR-0361 | ADR-1204 (Proposed); the verification lane in `docs/backends/metal/index.md` |
| `Ubuntu gcc static` | static CPU build, static pkg-config check, meson tests, tox | not required | fork-added before the ADR practice (`eaad70462`) | decision 2 |
| `Ubuntu CUDA static` | static CUDA build, CPU-only tests | not required | `eaad70462` | decision 2 |
| `Ubuntu SYCL` | icpx SYCL build, CPU-only tests | not required | `eaad70462` | decision 2 |
| `Ubuntu CUDA` | CUDA build, CPU-only tests | not required | `eaad70462` | removed by ADR-0689, restored by `384d97d03` |
| `Ubuntu SYCL+CUDA` | icpx SYCL and CUDA build, CPU-only tests | not required | `eaad70462` | decision 2 |
| `Windows MinGW64` | MSYS2 gcc static build, Win64 stack-alignment check, meson tests | required | ADR-0115, ADR-0116 | ADR-1234, ADR-1253, ADR-1254; removed by ADR-0691, restored by `384d97d03` |
| `Windows MSVC+CUDA` | MSVC and nvcc build, no tests | required | ADR-0121 | ADR-1234 |
| `Windows MSVC+SYCL` | MSVC and oneAPI build, no tests | required | ADR-0121 | ADR-1234 |

### `build.yml` (workflow `Build`)

These rows run on non-draft PRs and master pushes, and skip PRs that change
only documentation (`paths-ignore`).

| Lane | What it runs | Status | Owning record | Later ADRs, notes |
| --- | --- | --- | --- | --- |
| `Linux Intel LLVM` | icx/icpx build with CUDA, SYCL, HIP and DNN, meson dnn suite, meson tests, HIP smoke test; no tox | not required | ADR-0710 | GCC until #1161 (`195f88a22`); ADR-1185 |
| `macOS Clang+Metal` | CPU and Metal build, meson tests, tox | not required | ADR-0710 | ADR-1234 cites a failure it caught |
| `Windows MSVC+CUDA (full)` | MSVC and nvcc build, CPU tests | not required; renamed by this ADR, see below | ADR-0710 | — |

### Sanitizer and static-analysis gates (decision 3)

| Check | Workflow | Runs on | Status |
| --- | --- | --- | --- |
| `Sanitizers (address)`, `Sanitizers (undefined)`, `Sanitizers (thread)` | `tests-and-quality-gates.yml` | non-draft PRs, master pushes | required |
| `Cppcheck` | `lint-and-format.yml` | non-draft PRs, master pushes | required |
| `Sanitizers ASan+UBSan` | `sanitizers.yml` | non-draft PRs | not required |
| `Sanitizers TSan` | `sanitizers.yml` | master pushes | not required |
| `Fuzz <target>` | `sanitizers.yml` | nightly | not required |

ADR-0015 put TSan on a nightly schedule. The required `Sanitizers (thread)`
job runs on every PR, and decision 3 keeps it there. The rest of ADR-0015
stands, so it is not superseded.

### The shared `Windows MSVC+CUDA` check name

The aggregator looks required checks up by name and keeps one run per name,
the one that started last (`newestByName()` in `required-aggregator.yml`).
Since #1286 (`f93a0037f`) shortened the display names, the required ADR-0121
lane and the `build.yml` row both report as `Windows MSVC+CUDA`, so either can
hide a failure of the other. On master commit `7cc0cc91b` the `build.yml`
run was cancelled and the matrix run, which started one second later,
succeeded; only the success counts. `scripts/ci/check-aggregator-names.sh`
compared sets of names, so it could not see a duplicate. The maintainer chose
to rename the `build.yml` job (see References), which leaves the required name
and the ruleset untouched: it is now `Windows MSVC+CUDA (full)`, and
`check-aggregator-names.sh` fails when more than one job reports a required
name (`T-CI-MSVC-CUDA-SHARED-CHECK-NAME-2026-09-18` in `docs/state.md`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Record the running matrix and supersede the four ADRs (chosen) | Index, docs and CI agree; the later ADRs that depend on these lanes stay valid; no workflow change | Keeps the runner cost ADR-0710 set out to cut, and the overlap between `build.yml` and the matrix | — |
| Carry out ADR-0728 now | Eleven fewer build lanes per PR (its 14, less the Vulkan, MoltenVK and i686 rows already gone) | Removes required lanes that ADR-1225, ADR-1234, ADR-1253 and ADR-1254 depend on, and drops `Cppcheck`, which ADR-1245 and ADR-1246 extend | The maintainer chose to keep the lanes and the required checks (decisions 2 and 3) |
| Supersede ADR-0728 only | Smallest change to the record | ADR-0689, ADR-0691 and ADR-0710 would still read as in force although their removals were undone or never made | Leaves three of the four false records |
| Mark ADR-0728 Deprecated instead of Superseded | Says it never took effect | Gives no pointer to the record that is in force | A reader needs the successor |
| Leave the records as they are | No work | Contributors and agents keep acting on removals that never happened, as ADR-1234 did with the i686 lane | That is the problem this ADR fixes |

## Consequences

- **Positive**: the ADR index, the deprecations page, the unreleased changelog
  fragments and the workflow header comments describe the matrix that runs. A
  lane change now has one record to amend.
- **Negative**: every non-draft PR that touches the C core still runs 17 matrix
  lanes and the three `build.yml` rows, and `build.yml` largely repeats the
  matrix (`Linux Intel LLVM` against the SYCL, CUDA and HIP lanes,
  `macOS Clang+Metal` against `macOS Metal`, and the two Windows MSVC and
  CUDA jobs).
- **Neutral / follow-ups**: the unreleased changelog fragments that announced
  the ADR-0689, ADR-0691, ADR-0710 and ADR-0728 removals are removed or
  corrected, so the first VMAFx release notes do not announce them. Trimming
  the overlap is a separate decision.

## References

- Superseded: [ADR-0689](0689-vmafx-ci-matrix-dedupe.md),
  [ADR-0691](0691-vmafx-drop-legacy-build-paths.md),
  [ADR-0710](0710-vmafx-ci-slim-down-v2.md),
  [ADR-0728](0728-native-build-sunset.md).
- Related: [ADR-1258](1258-keep-64-bit-only-retire-i686-lane.md),
  [ADR-0015](0015-ci-matrix-asan-ubsan-tsan.md),
  [ADR-0115](0115-ci-trigger-master-only-and-matrix-consolidation.md),
  [ADR-0116](0116-ci-workflow-naming-convention.md),
  [ADR-0120](0120-ai-enabled-ci-matrix-legs.md),
  [ADR-0121](0121-windows-gpu-build-only-legs.md),
  [ADR-0212](0212-hip-backend-scaffold.md),
  [ADR-0313](0313-ci-required-checks-aggregator.md),
  [ADR-0361](0361-metal-compute-backend.md),
  [ADR-0726](0726-drop-vulkan-backend.md),
  [ADR-1140](1140-ci-impact-planner.md),
  [ADR-1225](1225-rocm-10-therock-migration.md),
  [ADR-1234](1234-local-preflight-gate.md),
  [ADR-1245](1245-cppcheck-exhaustive-configured-analysis.md),
  [ADR-1246](1246-cppcheck-public-entrypoints.md),
  [ADR-1253](1253-scalar-fma-not-fused-on-msvcrt.md),
  [ADR-1254](1254-win64-cannot-realign-the-stack.md).
- Commits: `0ab991afc` (#1567, ADR-0689), `9aa008e70` (#1564, ADR-0691),
  `384d97d03` (layout rename merge), `4e211736f` (#23, ADR-0710),
  `bfd4c436b` (#52, ADR-0728), `b53406e36` (upstream ARM lane), `eaad70462`
  (static, SYCL and CUDA lanes), `195f88a22` (#1161), `f93a0037f` (#1286).
- Popup, 2026-09-18, 32-bit x86: "Stay 64-bit only (ADR-0728)".
- Popup, 2026-09-18, lanes without a later ADR: "Keep them, record it
  (Recommended)".
- Popup, 2026-09-18, required checks: "Keep the four required (Recommended)".
- Popup, 2026-09-19, shared `Windows MSVC+CUDA` check name: "Rename build.yml's
  job (Recommended)".
