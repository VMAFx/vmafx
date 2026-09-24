<!-- markdownlint-disable MD013 MD060 -->
# BUG-003: REUSE coverage and provenance audit

**Date:** 2026-09-24
**Status:** Complete
**Related:** BUG-003, ADR-1250, PR #1539

## Question

Can the repository reach REUSE 3.3 compliance without assigning the fork's
EUPL-1.2 default to files whose copyright holders supplied them under other
terms?

The first coverage-only draft answered only the mechanical half of that
question. `reuse lint` proves that every file has syntactically valid copyright
and licence metadata. It does not prove that the named holder owns the work or
that the selected licence matches the work's history. A blanket root annotation
therefore made the command green while falsely classifying inherited, ported,
and outside-contributor work as Lusoris EUPL-1.2 work.

## Measured baseline

The baseline was current `origin/master` at `4e6916d16ac57647105d14a47a6680117d6b5738`.
The result is a snapshot of files in REUSE scope, not a count of all Git-tracked
paths.

| Check | Baseline | Corrected branch |
| --- | ---: | ---: |
| Files in REUSE scope | 9,137 | 9,140 |
| Files with copyright information | 2,331 | 9,140 |
| Files missing copyright information | 6,806 | 0 |
| Files with licence information | 1,886 | 9,140 |
| Files missing licence information | 7,251 | 0 |
| Invalid SPDX expressions | 14 | 0 |
| Unused licence texts | 1 | 0 |
| Missing, bad, or deprecated licence texts | 0 | 0 |

The corrected-branch count includes this digest. The exact final count is
re-measured after every content change; the invariant is zero missing or invalid
metadata, not a frozen repository size.

## Provenance audit

### Git history and upstream ancestry

The audit followed renames rather than inspecting only current paths. It built
two conservative sets:

1. current descendants of files inherited from Netflix/vmaf, including files
   renamed during the repository-layout migration; and
2. paths touched by an author or co-author other than the repository owner in a
   repository with neither a CLA nor a DCO.

The second set included files that began as Lusoris work but later received an
outside contribution, plus append-only aggregate files containing such work.
Every path in either set was compared with the effective `REUSE.toml` result.
After the exact overrides were added, both residual sets were empty:

- inherited or renamed upstream descendants classified as EUPL-1.2: **0**;
- no-CLA outside-contributor paths classified as EUPL-1.2: **0**.

The executable regression suite pins representative members of every
load-bearing group, including the six inherited root files, renamed upstream
descendants, PDFs with named authors, direct outside contributions, and
aggregates. The history-wide query remains an audit procedure because a future
commit necessarily changes its input set.

### FFmpeg patch stack

The patch files cannot inherit the fork default merely because they live in
this repository: a patch contains copyrighted portions of the source units it
changes. The stack was classified against the exact configured FFmpeg baseline,
official tag `n9.0.2` at commit
`946fcce07b6dcd0331c8cc609192aeff5e1924f8`, using that tree's `LICENSE.md` and
source headers.

- patches `0001` through `0018` touch LGPL-2.1-or-later source units or add
  files carrying LGPL-2.1-or-later headers;
- patch `0006` also retains Lawrence Curtis's copyright;
- patch `0019` spans both LGPL-2.1-or-later and GPL-2.0-or-later source units,
  so its conservative expression contains both terms.

Canonical LGPL-2.1-or-later and GPL-2.0-or-later texts were added under
`LICENSES/`. The effective-metadata tests pin all three patch classes so a
blanket default cannot silently return.

### Other retained terms

Exact annotations also preserve the existing terms for cJSON, Xiph, IQA,
`cpuid.asm`, `x86inc.asm`, Pelorus, FastDVDnet, the tiny-model artefacts, and
the already classified port/twin families from ADR-1250. The root default is
used only where the repository's existing provenance decision says the work is
fork-authored.

## Alternatives considered

| Option | Mechanical coverage | Provenance fidelity | Decision |
| --- | --- | --- | --- |
| One root EUPL-1.2 annotation with no exceptions | Complete | False for inherited, ported, and contributed work | Rejected even though `reuse lint` passes |
| Add a header or sidecar beside every uncovered file | Complete | Can be exact | Rejected: thousands of noisy edits obscure the substantive provenance review |
| Root default plus exact `override` groups | Complete | Exact for audited groups and regression-testable | Chosen |
| Exclude difficult files from the REUSE scope | Incomplete | Avoids answering the question | Rejected |

## Residual limits and maintenance rule

This is a provenance classification, not a legal opinion. The regression suite
protects known groups, but future history can create new groups. An upstream
sync, rename, new outside contribution, or new FFmpeg patch must be classified
before the coverage gate is considered meaningful. In particular:

- a green `reuse lint` result alone is never closure evidence;
- a new inherited or ported path needs an exact non-EUPL annotation;
- a contributed aggregate must retain every applicable prior term;
- a new FFmpeg patch must be checked against the configured upstream tag and
  every source unit it changes.

## Reproducer

```bash
reuse lint
pytest -q scripts/ci/tests/test_reuse_compliance.py
pre-commit run reuse-lint --all-files
pre-commit run test-reuse-compliance --all-files
```
