<!-- markdownlint-disable MD013 MD060 -->
# ADR-1298: Scope the local HISS audit's touched-file rule to what the committer wrote

- **Status**: Proposed
- **Date**: 2026-09-22
- **Deciders**: VMAFx maintainers
- **Tags**: `ci`, `agents`, `code-quality`

## Context

`praetorctl audit` applies a touched-file-must-be-clean rule on top of the
baseline ratchet, and derives the touched set from `git diff --name-only HEAD`
(`cmd/standardsctl/audit_ratchet.go:80` at the pinned engine).

During a merge, `HEAD` is still the pre-merge tip. That diff therefore names
every file the *merged-in side* changed, and the rule is applied to code the
committer never wrote.

Measured on the `#1518` restack, merging `origin/master` (at `7e20ab78d`, which
had just taken `#1507`'s ADM stack) into `integration/zero-warning-hiss21`:

```text
[FAIL] HISS invariant violations introduced
       (279 total infractions, 0 new unbaselined, 29 in touched files)
```

`0 new unbaselined` — the ratchet is satisfied. The 29 are pre-existing
baselined debt in three files master supplied verbatim:
`core/src/feature/x86/adm_avx2.c` (11), `adm_avx512.c` (11) and
`core/src/feature/sycl/integer_adm_sycl.cpp` (7) — `adm_decouple_s123_avx2` at
747 LOC, `adm_decouple_s123_avx512` at 682, `i4_adm_cm_avx2` at 551, and so on,
every one already carrying a cited `NOLINT`.

CI does not hit this and does not apply the rule at all. It runs the same bare
command on a clean checkout, where the diff against `HEAD` is empty, and
`.github/workflows/standards-gate.yml` records that as deliberate:

> Deliberately the baseline-only form, not `audit -base <ref>`. The `-base` mode
> adds a touched-file clean rule on top of the ratchet: every file a PR touches
> must carry zero HISS infractions. […] It would block routine fixes to legacy C
> for reasons unrelated to the fix. The ratchet still holds without it: recorded
> debt may shrink and never grow. Revisit `-base` once the baseline is low
> enough that touching a file is realistic (ADR-1249).

So the local hook was enforcing a rule the declared policy does not, purely as
an artefact of running against a dirty tree. The consequence is not confined to
one branch: under the repository's `strict_required_status_checks_policy`, every
pull request must be brought up to date with `master` before it can merge, so
any branch taking an update inherits master's debt in whatever files master
touched. `#1515` escaped only because a rebase replays commits one at a time and
none of its ten touched those files.

## Decision

`lefthook.yml`'s `hiss-audit` runs `scripts/git-hooks/hiss-audit.sh` instead of
a bare `praetorctl audit`.

- **Ordinary commit** — no `MERGE_HEAD`, nothing passed, `praetorctl` asks git
  exactly as before. Behaviour is unchanged, and the boy-scout rule keeps
  working on the files a commit actually edits.
- **Merge commit** — the touched set is the staged paths whose content differs
  from `MERGE_HEAD`'s version: the conflict resolutions, which is what the
  committer wrote. A file taken verbatim from the other parent is that parent's
  work, already covered by the ratchet and by whatever gate admitted it to
  `master`.

Measured on the same `#1518` merge: 56 staged paths, of which **21** differ from
what master supplied. Audited against those 21, the gate passes — every file the
committer actually wrote is clean, and the 29 findings were wholly in the 35
taken verbatim.

The engine is not reconfigured, no finding is suppressed, and the ratchet is
untouched: `0 new unbaselined` still fails the audit. `-allow-increase` and
`-touched-debt-delta-reason` remain unused, as the standing rule requires.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clear the 29 findings instead | No tooling change; the debt genuinely goes | Splitting 747/682/551/542-LOC AVX2/AVX-512 integer-ADM reduction bodies risks bit-exactness, and ADR-0141 §2 already cites the SYCL launch functions as cases where extracting a helper defeats device-kernel inlining | Large, risky, and unrelated to any change that triggers it |
| `praetorctl audit -base origin/master` in the hook | Uses the flag praetor provides for this | After a merge the merge base *is* `origin/master`, so the range becomes all ~190 commits of the branch — strictly worse. And the CI comment above says the `-base` mode reports 220 findings on an ordinary change set | Makes the problem bigger, and contradicts ADR-1249 |
| Drop the touched-file rule locally, matching CI exactly | Simplest; hook and CI then agree completely | Throws away a rule that has caught real debt on ordinary commits all through this branch's burn-down | Loses working enforcement to fix a merge-only defect |
| `-touched-debt-delta-reason` | Praetor ships it for provably mechanical changes | The standing rule in this repository is that it is not used, and a merge is not "provably mechanical" | Explicitly out of bounds |
| Rebase instead of merging | No merge commit, so no misderived touched set | Works for a ten-commit branch like `#1515`; rewriting ~190 commits on a branch with an open PR is not the same operation | Not general |
| Fix the derivation upstream in praetor | Correct at the root — praetor should read `MERGE_HEAD` | Cross-repository change, plus a release, a `PRAETOR_REF` bump on two branches and a re-record of master's 1367 baseline | Right long-term; this ADR does not preclude it, and the wrapper becomes a no-op once it lands |

## Consequences

- **Positive**: a branch can be brought up to date with `master` again, which
  `strict_required_status_checks_policy` requires of every pull request. The
  touched-file rule keeps its full force on ordinary commits.
- **Negative**: one more wrapper script between the hook and the engine, and the
  merge-time scope now depends on the wrapper being correct rather than on the
  engine. The script is short and its rule is stated in one sentence.
- **Neutral / follow-ups**: the root defect is praetor's, not this
  repository's — `audit_ratchet.go` should derive its touched set from the
  merge's own contribution when `MERGE_HEAD` exists. Worth filing upstream at
  `cordanaLLM/praetor`; this wrapper is then deletable.

## References

- Q: the maintainer chose "Align hook to declared policy" over clearing the 29
  findings or fixing praetor upstream, when shown that CI deliberately does not
  apply this rule.
- [ADR-1249](1249-praetor-governance-adoption.md) — the baseline-only decision
  the CI comment cites.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — the touched-file rule for
  clang-tidy, which is separate, gated by `Tidy Changed` and the ratchet, and
  unaffected.
