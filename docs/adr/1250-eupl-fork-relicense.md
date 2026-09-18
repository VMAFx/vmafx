<!-- markdownlint-disable MD013 MD060 -->
# ADR-1250: Fork-authored code moves to EUPL-1.2; code that carries someone else's work does not

- **Status**: Proposed
- **Date**: 2026-09-17
- **Deciders**: lusoris
- **Tags**: license, compliance, process, docs, breaking-change

## Context

The fork's licence metadata was wrong in three ways at once, found while measuring
the tree for REUSE compliance.

**An identifier that does not exist.** 946 files declared
`SPDX-License-Identifier: BSD-3-Clause-Plus-Patent`. There is no such SPDX
identifier; one further file declared `BSD+Patent`, the deprecated spelling of
`BSD-2-Clause-Patent`. A downstream tool reading either learns nothing.

**Several licences for one body of work.** Fork-authored files variously declared
`BSD-3-Clause-Plus-Patent`, `BSD-2-Clause-Patent`, `BSD-3-Clause`,
`BSD-3-Clause-Clear` and a dual `BSD-3-Clause-Plus-Patent OR MIT`; 223 more stated
their terms only in a prose block with no machine-readable tag, and 147 carried no
notice at all.

**No separation from upstream.** Netflix's code and the fork's own were not
distinguishable by metadata, so no consumer could tell which terms applied where.

Underneath the tidying is a deliberate choice about terms. The fork is now
substantially its own work, and the maintainer's intent is that this work be
reciprocally licensed rather than permissively donated.

## Decision

Fork-authored code is licensed under **EUPL-1.2**. Code the fork inherited, ported,
copied or translated from someone else keeps the terms it already carries, and
carries that code's copyright notice.

Which files move is decided by provenance, mechanically, never by reading header
text, and the decision is executable: `scripts/dev/relicense_fork_files.py` applies
it and `--check` proves the tree matches. A file moves only if it passes every one
of these, checked in order:

1. it is not under `compat/python-vmaf/`, which is Netflix's `python/vmaf`
   relocated wholesale by ADR-0700;
2. neither its path nor its pre-ADR-0700 `libvmaf/` path exists upstream;
3. no upstream file shares its name (names with no provenance signal, `__init__.py`
   and friends, are exempt);
4. it is not a verbatim mirror of another repository — the ten vendored Pelorus
   interop files (ADR-1113), whose terms are decided in VMAFx/pelorus and which
   `scripts/sync-pelorus-interop.sh` diffs byte for byte against that origin, so
   the tool never touches them;
5. it does not carry someone else's code — see below;
6. no copyright notice in it names anyone but Lusoris;
7. every licence it declares is one the fork has used for its own work;
8. no fork-local commit that touched it, following renames, was authored or
   co-authored by a person other than the owner.

Veto 5 is recorded in `scripts/dev/relicense_provenance.toml`, in three parts.
**Families** cover the SIMD and GPU kernels by role: a kernel implements the
upstream code that defines its metric whether or not its header says so, so every
`*_vif_*` kernel carries Netflix's VIF, every `ssimulacra2` kernel carries libjxl's,
every `psnr_hvs` kernel carries Xiph's. **Ports** list individually reviewed files
outside those directories — `ssimulacra2.c` ("scalar C port of the libjxl reference
implementation"), `speed_internal.c` ("a self-contained port of the static helpers
in speed.c"), tests whose scalar oracle is a transcription of an upstream static
function. **Not-ports** record the files a mechanical detector flagged and a human
cleared, with the reason, so the next run does not re-ask. A flagged file that
nobody has reviewed keeps its terms until someone does.

Veto 8 exists because the repository has no CLA and no DCO. Four SYCL files carry
contributions from Dmitry Popovich and two more were ported with Netflix
co-authors; those contributions arrived under the terms the files had then, and
relicensing them needs consent this decision does not have.

The 123 dual-licensed Go files become plain EUPL-1.2, dropping the MIT alternative,
because an MIT option lets a downstream take the permissive branch and defeat the
reciprocity this decision exists to create.

**This is a breaking change, deliberately.** Because `libvmaf.so` links fork code
and Netflix code together, and BSD-2-Clause-Patent is permissive enough to be
incorporated into a reciprocal work, the **combined library is effectively copyleft
from this point on**. Anyone distributing a modified libvmaf must offer source under
EUPL-1.2. Downstreams that need permissive terms can use Netflix upstream, which is
unaffected.

## What it did

| | Files |
| --- | --- |
| Moved to EUPL-1.2 | **1,514** |
| Kept their terms because they carry someone else's code, and gained its notice | **269** |
| Kept their terms: a copyright notice names someone else | 57 |
| Kept their terms: path or name exists upstream | 244 |
| Kept their terms: `compat/python-vmaf/` | 211 |
| Kept their terms: an outside contributor touched them | 5 |
| Kept their terms: vendored mirror of Pelorus, untouched | 10 |
| Not UTF-8, left alone | 9 |

Two repairs land on files that stay. An identifier that does not exist becomes the
one it meant, per ADR-1255. A file carrying someone else's code regains that code's
copyright notice and its licence joins the tag — `BSD-2-Clause-Patent AND
BSD-3-Clause` on the IQA-derived SSIM kernels, `AND MIT` on the CIEDE2000 kernels
that carry Joshua Holmer's maths, `AND BSD-2-Clause` on the Xiph-derived DCT. Those
licences all require the notice to travel with the code, and it had not been.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Decide by header text rather than provenance | Simpler to implement | Measured unreliable: the same tree yields wildly different "third-party" sets depending on the regex, and a fork-added file can still be a derivative that carries nobody's notice | Abandoned after an earlier attempt relicensed `ssimulacra2.c`, a C port of libjxl, and left 27 files with an EUPL tag stacked above a surviving BSD grant |
| Treat only same-language copies as carrying other code; move every GPU/SIMD reimplementation | Keeps EUPL over ~225 more files, including the GPU backends | A CUDA kernel that transcribes an upstream extractor's arithmetic is a translation of it; treating it as new work while its sibling with a more explicit header stays is a distinction without a difference | Rejected: siblings with identical provenance must be treated alike |
| EUPL-1.2 only outside `core/` and keep the C library permissive | The shipped library stays embeddable | The copyleft would apply only where nobody is tempted to take the code anyway | Rejected by the maintainer: the intent is reciprocity on the substantive work |
| Correct the invalid identifiers to `BSD-2-Clause-Patent` and stop | Smallest change | Leaves the fork permissively licensed, which is the thing being reconsidered | Addresses the typo, not the decision |

## Consequences

- **Positive**: every identifier in the tree is a real SPDX identifier; fork and
  inherited code are distinguishable by metadata; the fork's own work is
  reciprocally licensed; the four upstream and third-party projects whose code the
  fork carries are credited in the files that carry it, which their licences
  require; and the classification is reproducible by running one script rather than
  by rereading 2,300 headers.
- **Negative**: the shipped library is effectively copyleft, a breaking change for
  permissive consumers that must be prominent in the release notes; the MIT
  alternative is withdrawn from 123 Go files for future versions; contributors must
  be comfortable contributing under EUPL-1.2.
- **Neutral / follow-ups**:
  - The four SYCL files carrying Dmitry Popovich's contributions stay
    BSD-2-Clause-Patent. Moving them needs that contributor's agreement, which is a
    conversation, not a code change.
  - REUSE compliance is still not reached: most of the tree's data, fixtures and
    generated output carry no copyright information. A REUSE gate stays out of
    scope until that is addressed.
  - `relicense_fork_files.py --check` is not yet wired into CI. Until it is, a new
    fork-authored file can arrive with the wrong tag and nothing will say so.

## References

- [ADR-0105](0105-copyright-handling-dual-notice.md) — the copyright-header rule whose gate still passes over the rewritten tree.
- [ADR-0700](0700-vmafx-repo-layout.md) — the `libvmaf/` → `core/` move the provenance check has to account for.
- [ADR-1255](1255-spdx-residual-identifier-correction.md) — the identifier repair this decision applies to every file that stays.
- Source: user direction, 2026-09-15 — Netflix code keeps the current upstream licence and fork-added code moves to EUPL, chosen with the combined-work consequence stated.
- Source: user direction, 2026-09-16 — Q: how should files that port or copy someone else's code be treated? A: "All ports and twins stay", chosen after the measurement showed the rule reaches about 225 files rather than the 30–45 first estimated.
