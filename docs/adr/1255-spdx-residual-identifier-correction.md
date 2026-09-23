<!-- markdownlint-disable MD013 -->

# ADR-1255: Correct the SPDX identifiers PR #1457 does not reach, to the licence this repository already declares

- **Status**: Accepted
- **Date**: 2026-09-16
- **Deciders**: Lusoris
- **Tags**: license, docs, build

## Context

<!-- REUSE-IgnoreStart -->
1,027 tracked files declared `SPDX-License-Identifier: BSD-3-Clause-Plus-Patent`.
<!-- REUSE-IgnoreEnd -->
Checked against the current SPDX licence list, **no such identifier exists** —
the only patent-bearing BSD identifier SPDX defines is `BSD-2-Clause-Patent`
("BSD-2-Clause Plus Patent License"). There is no three-clause patent variant at
all, so the string appears to be a mash-up of `BSD-3-Clause` and
`BSD-2-Clause Plus Patent`. One further file carried `BSD+Patent`, the informal
spelling, which is likewise not an SPDX identifier.

A 1.0.0 release that declares a non-existent identifier fails every downstream
REUSE and SPDX validator, which is why the maintainer moved this ahead of rc.1
rather than leaving it with the rest of the REUSE work.

Most of it is already handled. PR #1457 (ADR-1250, on the `chore/eupl-fork-relicense` branch)
moves fork-authored code to EUPL-1.2 and, as a side effect, replaces the invalid
identifier on **935** of those files. Its three provenance vetoes deliberately
exclude the rest, leaving a residual of **92** files — documentation, `deploy/`
and `config/` YAML, `.claude/` skill templates, `ai/` scripts, Dockerfiles —
plus the one `BSD+Patent` occurrence.

The tree is also already inconsistent rather than uniformly wrong: **367 files
declare `BSD-2-Clause-Patent` correctly**, and the root `LICENSE` is
BSD-2-Clause-Patent. So the residual is not a question of choosing a licence for
files that lack one; it is a question of which of two identifiers already in use
in this repository the residual should carry.

## Decision

We correct the residual in the rc.1 integration train (#1425) to
`BSD-2-Clause-Patent`, the licence this repository declares in `LICENSE` and
already applies to 367 of its own files. The `BSD+Patent` occurrence — inside a
test fixture that mimics a Netflix header — is corrected the same way, so no
file anywhere in the tree contains an invalid SPDX identifier.

Only the identifier token is rewritten. The dual-licence structure of the Go
files (`... OR MIT`) is preserved, and the OCI
`org.opencontainers.image.licenses` labels in the Dockerfiles, which carry the
same invalid expression, are corrected with them.

This does **not** decide the licensing question ADR-1250 owns. If #1457 lands,
its provenance rule governs every file that passes its three vetoes, and it may
move some of these residual files to EUPL-1.2 on top of this correction. The two
are sequenced, not competing: this one makes the identifiers *valid*, ADR-1250
decides what they should *say*.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Correct the residual to `BSD-2-Clause-Patent` in the train (**chosen**) | Valid SPDX before rc.1 regardless of whether #1457 lands; matches `LICENSE` and the 367 files already using it; no new licensing assertion, since the identifier is already in use here | If #1457 lands it will rewrite some of these again, so the correction is partly transient | — |
| Fix the residual inside #1457 as EUPL-1.2 | All licensing in one PR; consistent with ADR-1250's rule for fork-authored files | rc.1's SPDX validity would then depend on #1457 landing first, and #1457 is a breaking change still under review | Couples a release blocker to a larger, riskier change |
| Fix all 1,027 in the train, not just the residual | Tree-wide validity immediately | Asserts permissive terms on 935 files ADR-1250 decided should be EUPL-1.2, and guarantees a large conflict with #1457 | Contradicts an Accepted ADR and pre-empts a decision that is not this PR's to make |
| Leave the identifiers and file an issue | Zero risk today | Ships 1.0.0 declaring a licence that does not exist | ADR-1250's own alternatives table already rejected this as "not a defensible resting state for a repository about to cut its first release" |

## Consequences

- **Positive**: no file in the tree declares a non-existent SPDX identifier.
  rc.1 can be validated by any SPDX or REUSE tool without a known-bad result,
  and that no longer depends on #1457's schedule.
- **Negative**: 938 identifier occurrences still read `BSD-3-Clause-Plus-Patent`
  and remain invalid until #1457 lands. **rc.1 is not SPDX-clean on the strength
  of this change alone** — this closes the part #1457 cannot reach, not the
  whole problem. Some of the corrected files may be rewritten again by #1457.
- **Neutral / follow-ups**: REUSE compliance proper (6,087 files with no
  copyright information at all) is unchanged and stays post-rc.1.

## References

- SPDX License List — `BSD-2-Clause-Patent` is the sole patent-bearing BSD
  identifier; verified against the published `licenses.json`, list version
  `1dd5767`.
- ADR-1250 (`chore/eupl-fork-relicense`, PR #1457) — the EUPL-1.2 relicensing decision
  and its three provenance vetoes, which define the residual this ADR covers.
- Related: `#1425`, `#1457`.
- Source: `Q1.1` (popup answer: "In the train (#1425), as BSD-2-Clause-Patent"),
  and `req` (paraphrased: the maintainer directed that the invalid SPDX
  identifiers be treated as pre-rc.1 work rather than deferred with the rest of
  REUSE).
