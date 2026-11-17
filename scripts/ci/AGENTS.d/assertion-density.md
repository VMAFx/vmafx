---
paths:
  - scripts/ci/assertion-density.sh
  - scripts/ci/tests/test-assertion-density.sh
  - scripts/ci/assertion-density-exempt-paths.txt
invariant: Every non-mirror C source is scanned; inherited/third-party files are exempt by exact path (sorted, validated, stale-listed paths fail); no name-based copyright filter.
area: tidy
---
<!-- markdownlint-disable MD013 MD060 -->
# assertion-density.sh — scan scope (ADR-0968, issue #3347)

`assertion-density.sh` scans **every non-mirror C source** the gate's
pathspecs list (`git ls-files ':(glob)core/src/**/*.c'
':(glob)core/src/**/*.cpp' ':(glob)core/tools/**/*.c'` filtered through
`scripts/ci/pelorus_mirror.py filter`). Fork ownership is decided by the
mirror filter, **never by the name in the copyright header**: a file an
outside contributor writes carries their own copyright line (ADR-3401)
and must still be scanned. The Lusoris header grep this file used to
document was removed in issue #3347 — do not reintroduce a name-based
filter, and do not widen the exemption to anything broader than exact
paths.

**Inherited and third-party sources** (upstream Netflix and its
NVIDIA co-authored CUDA files, Xiph, Daala, the IQA library, cJSON,
libsvm, dav1d — 82 files) are exempt by exact path through
`scripts/ci/assertion-density-exempt-paths.txt`. The manifest is loaded
with the shared `load_manifest()` in `scripts/ci/pelorus_mirror.py` (via
the `load-manifest` command), which **refuses an unsorted or duplicated
list** and validates every entry as a repository-relative exact path —
do not replace it with a second matcher or a prefix rule. The gate
**fails when a listed path no longer exists** in the source listing, so
an entry is pruned in the same change that removes its file and the
manifest cannot go stale silently.

2026-05-27 copyright-rebrand history (memory:
`project_copyright_lusoris_only`): the removed grep accepted both the
legacy `Lusoris and Claude (Anthropic)` format and the current
`Copyright YYYY Lusoris` format; the scope rule itself (which files are
checked) no longer depends on any of that — see the scope comment block
in the gate script.

Test coverage: `scripts/ci/tests/test-assertion-density.sh` (T1–T10) —
the rebrand fixtures (T1–T5), scan-by-default for contributor-named and
header-less files (T3–T6), exact-path exemption (T7), the stale-entry
failure (T8), the failing mirror filter (T9) and the top-level glob
(T10, #3390). The source listing runs in a checked command substitution:
a failing `git ls-files` or `pelorus_mirror.py filter` exits 2.
