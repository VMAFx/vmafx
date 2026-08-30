<!-- markdownlint-disable MD060 -->
# ADR-0493: Test YUV fixtures must be md5-verified, not just present-by-name

- **Status**: Accepted
- **Date**: 2026-05-17
- **Deciders**: lusoris
- **Tags**: testing, ci, fixtures, golden-data

## Context

The Python test suite (`python/test/quality_runner_test.py`,
`python/test/feature_extractor_test.py`) asserts numerical equality against
hardcoded golden values computed by Netflix for a specific pair of YUV
files: `src01_hrc00_576x324.yuv` and `src01_hrc01_576x324.yuv`. These two
fixtures back the §8 Netflix golden gate (CLAUDE.md / [ADR-0024](0024-netflix-golden-tests.md)).

Netflix removed both files from the upstream repo in 2020 (commit
`bac8b6073`) and moved them to a sibling repository:
<https://github.com/Netflix/vmaf_resource>. The fork's
`python/test/resource/yuv/` directory is `.gitignore`-d. CI provisions
the fixtures by inline `curl` in
[`.github/workflows/tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml).
Locally, there has been no equivalent provisioner; developers were
expected to either run CI or hand-place the files.

This left a sharp edge: a local copy of either file with the right
*name* and *size* but the wrong *content* would silently produce wrong
test results. On 2026-05-17, the working tree was found to contain two
such stale copies (md5 `4226fb7e…` / `89c83814…` instead of
`b16f67d3…` / `2e02bed1…`). This produced 84 test failures whose
assertion messages showed only "actual ≠ expected"; the actual VIF
score was almost 2× the expected value, which read as a feature-
extractor bug. The root cause was a fixture-content mismatch.

A previous PR ([#1237](https://github.com/VMAFx/vmafx/pull/1237))
updated the ADM2 golden from `0.9345` to `0.9878` based on the output
the (stale) fork was producing, on the assumption that the prior value
was a stale hardcode. That commit is reverted as part of the same PR
introducing this ADR: with canonical fixtures, the C extractor produces
exactly `0.9345057708333333`, matching upstream's golden.

## Decision

We will treat **test fixture md5 sums as part of the test contract**,
not just file presence. Specifically:

1. Ship `scripts/test/fetch-test-yuvs.sh` as the canonical local
   provisioner. It downloads each required YUV from
   `Netflix/vmaf_resource` and verifies its md5 against a hardcoded
   expected value. Existing local copies are md5-checked; mismatches
   are refetched, not silently trusted.
2. Document the provisioner in
   [`docs/development/test-fixtures.md`](../development/test-fixtures.md)
   as the single, discoverable entry-point for "I just cloned the repo
   and want to run `pytest python/test/`."
3. When CI updates the inline `curl` URLs (e.g. if `vmaf_resource`
   reorganises), the script's hardcoded md5 list must be updated in
   the same change. The pre-commit hook
   [`check-fixture-md5`](../../scripts/ci/check-fixture-md5.sh) is
   out-of-scope for this ADR and tracked separately; for now the
   bidirectional sync is documented and enforced by reviewers.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Track YUVs via git-lfs | Always present at clone time; no provisioner needed | Adds LFS bandwidth/storage cost; ~13 MB per file × 2 files (+ checkerboards if extended); duplicates state that already lives at vmaf_resource | The fixtures already live upstream in a versioned form; LFS would mirror them without adding integrity guarantees |
| Commit binary YUVs into the repo | Zero provisioning friction | Bloats clone, duplicates upstream, requires repo bloat-cleanup if content changes | Same as LFS but worse; rejected on the same grounds upstream had in 2020 |
| Document the CI curl as a copy-paste recipe | Cheapest in lines-of-code | No md5 verification → the exact same silent-corruption failure mode that this ADR exists to prevent | Recipe-in-prose loses the md5 check, which is the whole point |
| Auto-run the provisioner inside `conftest.py` | Pytest "just works" with no extra step | Hides network I/O inside test discovery; surprises users running tests behind a firewall or air-gapped | Make the provisioning step explicit and discoverable |

## Consequences

- **Positive**: a single command (`scripts/test/fetch-test-yuvs.sh`)
  brings a fresh clone into a state where `pytest python/test/` runs
  against canonical fixtures. Silent fixture corruption is detected
  at provision time, not as a confusing test failure later.
- **Positive**: PR [#1237](https://github.com/VMAFx/vmafx/pull/1237)'s
  ADM2 override is reverted; CLAUDE.md §8 is restored to "never modify
  Netflix golden assertions" with no live exception.
- **Negative**: a third place (script, CI workflow, ADR) now records
  the canonical md5 list. When `vmaf_resource` updates a fixture, all
  three must move together.
- **Neutral**: extensions to cover the checkerboard pairs and
  `KristenAndSara_1280x720_8bit_processed.yuv` are deferred until
  they are needed by a CI-run test; the current script covers the
  fixtures actually exercised by CI today.

## References

- Upstream removal commit: `bac8b6073` ("Remove python/test/resource/yuv files.")
- Canonical fixture source: <https://github.com/Netflix/vmaf_resource>
- CI download path: `.github/workflows/tests-and-quality-gates.yml`
- Reverted PR: [#1237](https://github.com/VMAFx/vmafx/pull/1237)
- Related ADRs: [ADR-0024](0024-netflix-golden-tests.md) (Netflix golden gate)
- Source: req (user investigation 2026-05-17 — "Investigate integer-VIF first")
