# Research-0892: Conventional-Commits + changelog-fragment hygiene audit

- **Status**: Complete
- **Date**: 2026-05-30
- **Companion ADR**: [ADR-0892](../adr/0892-conventional-commits-and-changelog-fragment-hygiene.md)
- **Branch tip audited**: `master@83698bd5b2`
- **Worktree**: `/tmp/wt-cc-audit` (`chore/conventional-commits-audit`)

## 1. Scope

Validate that:

1. Recent commits on `origin/master` conform to Conventional Commits
   (so release-please can derive the right section assignment).
2. Every Conventional-Commits type used on master has a corresponding
   `changelog-sections` entry in `release-please-config.json`.
3. Every directory under `changelog.d/` is one of the documented
   Keep-a-Changelog sections (`added`, `changed`, `deprecated`,
   `removed`, `fixed`, `security`) per ADR-0221.
4. Every fragment file actually reaches the rendered
   `CHANGELOG.md` Unreleased block via
   `scripts/release/concat-changelog-fragments.sh`.

## 2. Method

```bash
git worktree add -b chore/conventional-commits-audit /tmp/wt-cc-audit origin/master
cd /tmp/wt-cc-audit
git log --pretty=format:'%s' HEAD > /tmp/commit-subjects.txt   # 50 subjects (squash history)
# validation regex: ^(type)(\(scope\))?!?: subject$
# allowed types: feat, fix, docs, chore, refactor, test, perf, style, build, ci, revert, security
python3 /tmp/cc-check.py                        # type distribution + violations
python3 /tmp/cc-cross.py                        # cross-check used vs configured sections
find changelog.d -mindepth 1 -maxdepth 1 -type d
for d in changelog.d/*/; do echo "$d: $(find "$d" -maxdepth 1 -name '*.md' | wc -l)"; done
```

## 3. Findings

### 3.1 Commit-message lane (50 commits scanned)

| Type | Count | In root release-please section? |
| --- | --- | --- |
| `fix` | 32 | yes |
| `chore` | 8 | yes |
| `docs` | 5 | yes |
| `ci` | 2 | yes |
| `test` | 1 | yes |
| `feat` | 1 | yes |
| `revert` | 1 | **no — gap** |

All 50 subjects are well-formed Conventional Commits. **Zero format
violations.** One commit (`d0b697c6` —
`revert: drop continue-on-error on release-please now that org enables
Actions→PR (#280)`) uses the `revert` type, which is not present in
the root package's `changelog-sections` list. Release-please's
behaviour on an unconfigured type is to drop the commit from the
generated CHANGELOG (no section assignment, no fallback to
"Miscellaneous"). The two other standard Conventional-Commits types
also missing from the root section list are `security` and `style` —
neither appeared on master in the audit window, but both are valid
and the next commit using them would be silently dropped.

### 3.2 Fragment-tree lane

| Directory | File count | Documented? | Renderer iterates? |
| --- | --- | --- | --- |
| `changelog.d/added/` | 286 | yes (ADR-0221) | yes |
| `changelog.d/changed/` | 214 | yes (ADR-0221) | yes |
| `changelog.d/fixed/` | 330 | yes (ADR-0221) | yes |
| `changelog.d/removed/` | 3 | yes (ADR-0221) | yes |
| `changelog.d/security/` | 5 | yes (ADR-0221) | yes |
| `changelog.d/perf/` | 27 | **no — not in ADR-0221, not iterated** | **no** |
| `changelog.d/performance/` | 5 | **no — not in ADR-0221, not iterated** | **no** |

The renderer's section list is a hard-coded bash array:

```bash
SECTIONS=(added changed deprecated removed fixed security)
SECTION_TITLES=(Added Changed Deprecated Removed Fixed Security)
```

— so any file dropped into `perf/` or `performance/` is **silently
discarded** at render time. 32 contributor-written entries describing
real performance work merged to master would never have appeared in
the rendered `Unreleased` block. The mistake is understandable: the
Conventional-Commits spec lists `perf` as a recognised type, and
`release-please-config.json` even has a `{ "type": "perf", "section":
"Performance" }` row, so the perf-as-its-own-section mental model is
encoded in the commit-message lane. But the fragment lane is
governed by ADR-0221, which froze the list to the six
Keep-a-Changelog sections.

### 3.3 Cross-pipeline consistency

The two lanes (commit-message via release-please, fragment-file via
the in-tree bash renderer) currently disagree:

| Lane | Performance handling |
| --- | --- |
| release-please (commits) | `perf: …` commits → `### Performance` section |
| fragment renderer | no `Performance` section; `perf/` dir ignored |

The fragment-side lane is the authoritative one per ADR-0221 (the
Unreleased block is rendered from fragments, not from commits).
The commit-message-derived sections only apply when release-please
cuts a version tag and rolls the Unreleased block into a versioned
section. The two pipelines must agree at that point, otherwise the
versioned changelog will have a `### Performance` heading derived
from commit types but no entries derived from fragments.

## 4. Decisions (applied in this PR)

1. **`release-please-config.json` extended** (root + `ai` packages)
   to add `revert → Reverts`, `security → Security`, `style → Style`
   so the three orphan standard types map to explicit sections. The
   two smaller python packages (`dev-llm`, `mcp-server/vmaf-mcp`)
   keep their narrow type lists — their commit traffic doesn't see
   these types in practice and adding them would be over-scoping.
2. **`changelog.d/perf/` and `changelog.d/performance/` removed**;
   all 32 entries moved to `changelog.d/changed/` with `perf-`
   filename prefix so they sort together inside the rendered
   `### Changed` section. In-fragment `## Performance` /
   `### Performance` / `## perf(…)` headings stripped (the renderer
   emits `### Changed` itself); other in-body headings demoted to
   bold-prefixed bullets so they sit cleanly under the rendered
   section heading.
3. **`changelog.d/README.md` updated** to explicitly call out that
   performance entries belong in `changed/perf-<topic>.md`, and that
   the six Keep-a-Changelog directories are the only valid section
   dirs.
4. **`docs/rebase-notes.md` entry added** flagging the rename for
   any in-flight feature branches that touch a moved fragment.

## 5. Out of scope

- The rendered `CHANGELOG.md` Unreleased block is already drifted
  against the fragment tree (pre-existing condition,
  `scripts/release/concat-changelog-fragments.sh --check` exits 1).
  This audit does not re-render the body because the drift predates
  the audit and includes content beyond performance entries; a
  separate sweep PR can run `--write` once this PR lands and the
  fragment tree is invariant-clean.
- Wiring `--check` as a required CI status. Documented as a
  follow-up in ADR-0892 §Consequences; not addressed here to keep
  the diff focused.

## 6. Reproduction

```bash
git fetch origin
git checkout chore/conventional-commits-audit
# 1. validate commit subjects
git log --pretty=format:'%s' origin/master | head -50 | \
  grep -vE '^(feat|fix|docs|chore|refactor|test|perf|style|build|ci|revert|security)(\([a-z0-9._\-/]+\))?!?: '
# expected: empty output (zero violations)

# 2. confirm renderer's section list
grep '^SECTIONS=' scripts/release/concat-changelog-fragments.sh
# expected: SECTIONS=(added changed deprecated removed fixed security)

# 3. confirm no orphan dirs remain
ls -d changelog.d/*/ | grep -vE 'changelog.d/(added|changed|deprecated|removed|fixed|security)/'
# expected: empty output

# 4. confirm release-please covers all standard CC types
jq -r '.packages["."]["changelog-sections"][] | .type' release-please-config.json | sort
# expected: includes revert, security, style alongside existing types
```
