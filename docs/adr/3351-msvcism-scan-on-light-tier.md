# ADR-3351: The msvcism scan is a required step of every pull request

- **Status**: Accepted
- **Date**: 2026-10-18
- **Deciders**: Lusoris
- **Tags**: ci, build, testing

## Context

Pull request #2852 added five `nullptr` to a C test
(`core/test/test_pelorus_udu_wrap.c`) and merged with every required check
green. Master then failed on `Windows ARM64 MSVC` and `Windows MSVC+CUDA (full)`
with C2065: MSVC's `/std:clatest` C23 feature set has no `nullptr`, which is
why [ADR-1138](1138-c-translation-units-keep-null.md) keeps C on `NULL`.

Two gaps let it through:

- The pull request ran the light CI tier ([ADR-2169](2169-ci-fewer-runs.md)).
  The only MSVC leg a light pull request builds is `Windows MSVC+SYCL`
  ([ADR-3279](3279-msvc-sycl-leg-on-light-tier.md)), which compiles with
  `icx-cl`, a clang front end that accepts `nullptr` in C. The two `cl.exe`
  legs are full-tier, and the required `Windows MSVC+CUDA` context reports
  success when its full-tier leg is skipped.
- The static substitute, the `msvcism` stage of `scripts/dev/preflight.sh`,
  ran in no CI job, and it threw its findings away. Each scan ran one grep per
  file; a clean file makes grep exit 1, pipefail then failed the pipeline, and
  a `|| hits=""` fallback replaced every finding with nothing whenever the last
  scanned file was clean. Run against #2852's base, the stage passed with all
  five hits. The static-aggregate scan also looked up its scanner under the
  scanned repository's root and did not run outside this repository.

## Decision

The required `Pre-Commit` job (light tier, every pull request and push) runs
`scripts/dev/preflight.sh --stage msvcism` on the files changed against the
pull request's base. Every scan of the stage treats a per-file grep status of
1 as a clean file, ends on any other status, and fails the stage when it does
not finish; no scan resets its findings. The scanners are found next to the
preflight script.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Build a `cl.exe` leg on every light pull request that changes C | the compiler itself is the check | ARM64 MSVC and MSVC+CUDA take tens of minutes on hosted Windows runners; ADR-3279 chose one leg for that reason | cost on every C pull request |
| Swap the light-tier MSVC leg from icx-cl to cl.exe | one leg, real MSVC front end | loses the SYCL build ADR-3279 measured as the one with most Windows-only failures | trades one blind spot for another |
| Run the msvcism stage in Pre-Commit (chosen) | seconds; the known MSVC rejection classes are greppable; reuses the existing preflight stage and its self-test | a static scan, not a compiler: an unknown rejection class still reaches master | — |

## Consequences

- **Positive**: a `nullptr` in a changed C file, and every other msvcism class,
  fails a required check on the pull request. The stage's self-test
  (`scripts/ci/tests/test-preflight-msvcism.sh`, run by `make lint-sh`) holds
  a finding followed by a clean file and the static-aggregate scan to a failing
  result; both cases fail against the previous script.
- **Negative**: the scan can be wrong in both directions; `Windows MSVC+*`
  stays the authority on the full tier and on master.
- **Neutral / follow-ups**: a pull request that needs the `cl.exe` legs before
  merging still takes the `ci: full` label.

## References

- Order `vmafx-main-869c2e6-msvc-nullptr-20261018` (controller ledger, 2026-10-18,
  paraphrased): fix the master failure and close the gap that let the legs not
  gate #2852, with a planted C23-only keyword that turns the pull request red.
- [ADR-1138](1138-c-translation-units-keep-null.md),
  [ADR-2169](2169-ci-fewer-runs.md),
  [ADR-3279](3279-msvc-sycl-leg-on-light-tier.md).
