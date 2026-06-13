# ADR-0117: Coverage-Gate annotation cleanup (gcov hits + upload-artifact)

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, coverage, gcovr, github-actions

## Context

The Coverage Gate job ([.github/workflows/ci.yml](../../.github/workflows/ci.yml))
finished green after [ADR-0114](0114-coverage-gate-per-file-overrides.md), but
the GitHub Actions Annotations panel still surfaced three warnings on every
run:

1. `Node.js 20 actions are deprecated. The following actions are running on
   Node.js 20 ... actions/upload-artifact@v5 ...` — emitted by GitHub's
   runner against any action that still ships a Node 20 entry point. Forced
   migration to Node 24 starts 2026-06-02.
2. `(WARNING) Ignoring suspicious hits in
   .../core/src/feature/ansnr_tools.c:207: 4932845568: 207-block 1.` —
   gcovr 8 flagging a hit count above its built-in sanity threshold (~2³² /
   4.29 G). The line is the inner loop of the ANSNR border-replicate
   convolution; on a single 1080p frame that loop runs `1920 × 1080 × 5 × 5
   ≈ 259 M` times, so a multi-frame coverage suite trivially clears 4.93 G
   real iterations. Not a gcov instrumentation bug.
3. Same warning, different gcovr emit format for the same line. Counts as a
   separate annotation.

All three are noise — the data is correct, the code is correct, the action
still works. But annotation panels are a quality-of-life surface; reviewers
who land on a Coverage Gate run with three yellow triangles waste cycles
deciding whether they're real.

## Decision

Two coordinated changes, both applied to **every** workflow under
`.github/workflows/`:

1. **Bump `actions/upload-artifact@v5` → `@v7`** (and `@v6` → `@v7` on
   `libvmaf.yml` — already mid-bump). v7 ships with Node 24 native, which
   silences the GitHub-runner deprecation banner. Also bump
   `actions/download-artifact@v5` → `@v7` in [supply-chain.yml](../../.github/workflows/supply-chain.yml)
   so signing/SBOM jobs keep matching the upload format.

2. **Pipe gcovr stderr through `grep -vE 'Ignoring (suspicious|negative)
   hits'`** for both gcovr invocations in [ci.yml](../../.github/workflows/ci.yml)
   (CPU coverage step + GPU coverage step). The
   `--gcov-ignore-parse-errors=suspicious_hits.warn` flag still tells
   gcovr to *accept* the data (we want the hit counts in the report); the
   stderr filter only drops the chatty annotation line. `|| true` after
   grep guards against process-substitution propagating exit-1 when the
   filter matches nothing on a clean run.

The stderr filter is intentionally narrow (regex anchored to gcov's exact
warning prefix) so any *other* gcovr warning still surfaces. If a future
gcov change emits a genuinely suspicious overflow we want to see, the
filter won't hide it — we'd see "WARNING" lines that don't match the
suspicious/negative pattern.

## Alternatives considered

1. **Use `--gcov-ignore-parse-errors=suspicious_hits.warn_once_per_file`
   instead of stderr filter.** Reduces 2 warnings → 1 per file, but
   doesn't get to zero. The user request was zero noise on the
   annotations panel; "fewer" doesn't satisfy. We'd still need a stderr
   filter on top.
2. **Patch gcovr to add a true silent-ignore mode and upstream it.**
   Right thing in the long run; out of scope for this PR. Filed as a
   reminder via this ADR. When/if gcovr ships `suspicious_hits.ignore`,
   swap the stderr filter for the proper flag and update this ADR.
3. **Hard-code the gcov instrumentation to skip ansnr_tools.c via a
   `GCOV_PROFILE_ARCS_OFF` pragma.** Loses coverage data for one of the
   two ANSNR feature extractors. Net negative — the warning is benign,
   the coverage data is useful.
4. **Pin to a specific upload-artifact commit SHA instead of `@v7`.** SHA
   pinning is more reproducible and OpenSSF-Scorecard-friendly, but the
   rest of the repo is on `@v<major>` tags throughout (see
   `actions/checkout@v6`, `actions/setup-python@v6`). Consistent floating
   tag is the existing convention; SHA-pinning is a separate ADR
   waiting to be written for the whole tree.
5. **Defer to a "no warnings" CI gate that fails on any annotation.**
   Tempting forcing-function but currently impossible — a fresh repo
   spin-up emits transient `actions/checkout` warnings on flaky DNS that
   we don't want failing builds. Worth revisiting once we have a stable
   stderr-allowlist mechanism.

## Consequences

**Positive:**

- Coverage Gate now finishes with **zero** annotations on a clean run.
  Reviewers see green-and-empty, which is the actual signal.
- Forward-compat with the GitHub Node 24 forced migration on
  2026-06-02; no rush-fix PR needed when the runner flips the deprecation
  banner to a hard error.
- Stderr filter is a one-line addition per gcovr invocation; trivially
  removable when gcovr ships a proper silent-ignore.

**Negative:**

- The stderr filter could mask a *real* future gcov bug if it ever emits
  a "suspicious hits" warning for a line that genuinely has bad
  instrumentation data. Mitigation: the filter is regex-narrow, so any
  warning with different wording still surfaces. We accept the residual
  risk that gcov's heuristic might one day catch a real instrumentation
  issue we'd want to see — at which point we'd see no annotation,
  investigate the coverage drop directly, and either narrow the filter
  or remove it.
- `actions/upload-artifact@v7` ships with the immutable-artifacts
  semantics introduced in v6 — re-uploading to the same artifact name in
  a single workflow run now requires `overwrite: true`. The repo doesn't
  re-upload to the same name anywhere today (verified by grepping each
  workflow), so no functional break.

**Neutral:**

- `actions/upload-pages-artifact@v5` and `actions/deploy-pages@v5` in
  [docs.yml](../../.github/workflows/docs.yml) are *separate* actions
  with their own versioning, and both currently use Node 24. Left at
  `@v5` — they don't trigger the deprecation banner.

## References

- [ADR-0110](0110-coverage-gate-fprofile-update-atomic.md) — gcov
  `-fprofile-update=atomic` baseline that produced the high hit counts in
  the first place.
- [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) — `lcov → gcovr`
  migration; this ADR is a follow-on quality-of-life fix on top of that
  switchover.
- [ADR-0114](0114-coverage-gate-per-file-overrides.md) — last gate
  change; took the suite to green-with-warnings. This ADR takes it to
  green-without-warnings.
- `req` (paraphrased): user reported "still missed warnings" on the
  Coverage Gate Annotations panel after the ADR-0114 merge, with a
  screenshot showing the three warnings enumerated above.
- gcovr 8.x docs on `--gcov-ignore-parse-errors`:
  <https://gcovr.com/en/stable/manpage.html>
- GitHub Actions Node 20 deprecation announcement:
  <https://github.blog/changelog/2024-09-24-actions-node-20-deprecation/>
- Per-surface doc impact: this ADR documents the workflow-file change.
  No user-facing CLI surface changed; no `docs/` topic-tree entry needed
  beyond the ADR itself.
