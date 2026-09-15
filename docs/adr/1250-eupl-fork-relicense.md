<!-- markdownlint-disable MD013 MD060 -->
# ADR-1250: Fork-authored code moves to EUPL-1.2; Netflix-derived code keeps its licence

- **Status**: Proposed
- **Date**: 2026-09-15
- **Deciders**: lusoris
- **Tags**: license, compliance, process, docs, breaking-change

## Context

The fork's licence metadata was in three kinds of disarray at once, discovered
while measuring the tree for REUSE compliance.

**An identifier that does not exist.** 946 files declared
`SPDX-License-Identifier: BSD-3-Clause-Plus-Patent`. There is no such SPDX
identifier. One further file declared `BSD+Patent`, the deprecated spelling of
`BSD-2-Clause-Patent`. `reuse lint` cannot resolve either, and a downstream tool
reading them learns nothing about the terms.

**Three licences for one body of work.** Fork-authored files variously declared
`BSD-3-Clause-Plus-Patent`, `BSD-2-Clause-Patent`, `BSD-3-Clause`,
`BSD-3-Clause-Clear`, `Unlicense`, and 123 Go files carried a dual
`BSD-3-Clause-Plus-Patent OR MIT`. 223 more stated their terms only in a prose
block ("Licensed under the BSD+Patent License") with no machine-readable tag at
all, and 147 carried no licence notice of any kind.

**No licence separation from upstream.** Netflix's code and the fork's code were
not distinguishable by metadata, so no consumer could tell which terms applied to
which file.

Underneath the tidying sits a deliberate choice about terms. The fork is now
substantially its own work — 1,622 files here have no counterpart in Netflix
upstream — and the maintainer's intent is that this work be reciprocally
licensed rather than permissively donated.

## Decision

We will licence fork-authored code under **EUPL-1.2** and leave every
Netflix-derived and third-party file on the terms it already carries. A file
moves only if it passes three independent, mechanical vetoes: its path has no
counterpart in `upstream/master` (including the ADR-0700 `libvmaf/` → `core/`
remap, with `compat/python-vmaf/` excluded as a tree because it is Netflix's
`python/vmaf` relocated); no file of the same name exists anywhere upstream; and
it carries no copyright line other than Lusoris. The 123 dual-licensed Go files
become plain EUPL-1.2, dropping the MIT alternative, because an MIT option would
let a downstream user take the permissive branch and defeat the reciprocity this
decision exists to create.

This is a breaking change for consumers, and deliberately so: because
`libvmaf.so` links fork code and Netflix code together, and BSD-2-Clause-Patent
is permissive enough to be incorporated into a reciprocal work, the **combined
library is effectively copyleft from this point on**. Anyone distributing a
modified libvmaf must offer source under EUPL-1.2. Downstreams that need
permissive terms can use Netflix upstream, which is unaffected.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| EUPL-1.2 only outside `core/` (tools, Go services, AI) and keep the C library permissive | The shipped library stays embeddable; FFmpeg and commercial consumers unaffected; no combined-work question | The copyleft would apply only where nobody is tempted to take the code anyway; the library — the actual contribution — stays permissively donated | Rejected by the maintainer after the combined-work consequence was stated explicitly: the intent is reciprocity on the substantive work, not on the periphery |
| Correct the invalid identifiers to `BSD-2-Clause-Patent` and stop | Smallest change; nothing about distribution changes | Leaves the fork permissively licensed, which is the thing being reconsidered | Does not address the decision, only the typo |
| Leave the identifiers alone and file an issue | Zero risk today | 946 files keep asserting a licence identifier that does not exist, and REUSE compliance stays unreachable | Not a defensible resting state for a repository about to cut its first release |
| Relicence by header text rather than by provenance | Simpler to implement | Measured unreliable: the same tree yielded 6 or 324 "third-party" files depending on the regex, and a fork-added file can still be a derivative work carrying somebody else's notice | Abandoned after it relicensed `cambi_neon.c` (Netflix copyright) and prepended a contradictory header above `ssimulacra2.c`'s existing grant |

## Consequences

- **Positive**: every identifier in fork-authored code is a real SPDX identifier;
  fork and upstream code are distinguishable by metadata; the fork's substantive
  work is reciprocally licensed; `reuse lint` can resolve every licence the
  fork's own files reference.
- **Negative**: the shipped library is effectively copyleft, which is a breaking
  change for permissive consumers and must be prominent in the release notes; the
  MIT alternative is withdrawn from 123 Go files for future versions; contributors
  must now be comfortable contributing under EUPL-1.2.
- **Neutral / follow-ups**:
  - **630 files were deliberately not touched** and keep their current terms: 241
    in `compat/python-vmaf/` (Netflix's relocated Python tree), 264 whose path
    exists upstream, 107 carrying a third-party copyright line (the arm64 NEON
    kernels hold Netflix's, `ssimulacra2.c` holds libjxl's), 9 matching an
    upstream filename, and 9 that are not UTF-8.
  - **95 files still declare the invalid `BSD-3-Clause-Plus-Patent`**, and one
    declares `BSD+Patent`. All are in the excluded set, so correcting them means
    determining the real terms of a third-party derivative — its own change, not
    a search and replace.
  - REUSE compliance is still not reached: 6,087 files carry no copyright
    information, most of them data, fixtures and generated output rather than
    source. A REUSE gate remains out of scope until that is addressed.
  - `LICENSES/Apache-2.0-u2netp.txt` is not a valid SPDX identifier and should
    become `LicenseRef-`-prefixed.
  - The `ssimulacra2` EOTF LUT generator emitted the old prose block into its
    generated header; it now emits the tag, verified by regenerating and diffing.

## References

- [ADR-0105](0105-copyright-handling-dual-notice.md) — the copyright-header rule whose gate
  still passes over the rewritten tree.
- [ADR-0700](0700-vmafx-repo-layout.md) — the `libvmaf/` → `core/` move the
  provenance check has to account for.
- Netflix upstream remains BSD-2-Clause-Patent; nothing in this change alters the
  terms of any file the fork inherited.
- Source: user direction, 2026-09-15 — Netflix code keeps the current upstream
  licence and fork-added code moves to EUPL, chosen with the combined-work
  consequence stated.
