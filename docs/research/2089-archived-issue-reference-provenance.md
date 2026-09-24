<!-- markdownlint-disable MD013 -->
# Research-2089: Archived issue-reference provenance after the repository migration

- **Date**: 2026-09-24
- **Status**: Active
- **Scope**: BUG-048 Section E issue-reference provenance only
- **Base**: collector `719ae5fc424d70e9a4612ca1db293e0568988ac6`

## Question

Which bare issue and pull-request numbers in the current tree describe the
pre-migration `lusoris/vmaf` tracker, and how can the repository prevent those
identities from silently becoming links in the newer `VMAFx/vmafx` number
space without banning ordinary fork-local issue references?

## Repository evidence

Commit `2581c7ba0` qualified five established historical identities in
`docs/state.md`:

| Historical identity | Recorded subject | Resolving history |
| --- | --- | --- |
| `lusoris/vmaf#239` | FFmpeg `libvmaf_vulkan` synchronous-fence serialisation | `e266bf8e` (`lusoris/vmaf#241`) |
| `lusoris/vmaf#310` | ADR-number collision sweep cited by the Vulkan row | `af227b026` |
| `lusoris/vmaf#752` | second ADR-number collision sweep | `fb14bc332` |
| `lusoris/vmaf#857` | `cambi_cuda` invalid kernel parameter / SIGSEGV | `37fcfea68` (`lusoris/vmaf#866`) and follow-up `8de50984a` |
| `lusoris/vmaf#870` | CUDA CAMBI host-preprocessing correction | `8de50984a` |
| `lusoris/vmaf#407` | BVI-DVC corpus ingestion and `merge_corpora.py` | `a159ba9da` |

The unrelated vendored-test commit `d468c5bb4` later restored the prior bare
spellings in `docs/state.md`; it did not explain or supersede the provenance
decision. `git show 2581c7ba0 -- docs/state.md` and
`git show d468c5bb4 -- docs/state.md` are a direct add/remove pair.

The active repository now reuses the original five ledger numbers for unrelated pull requests.
The GitHub API on 2026-09-24 identifies, for example, active `VMAFx/vmafx#239`
as the Cython source-root rename, `VMAFx/vmafx#241` as the legacy-runner test
sunset, `VMAFx/vmafx#310` as Go context propagation, `VMAFx/vmafx#857` as a
later medium/low fix bundle, and `VMAFx/vmafx#870` as a later Python CodeQL
cleanup. Active `VMAFx/vmafx#866` is a later Fable defect bundle, not the CAMBI
dispatch fix in historical `lusoris/vmaf#866`. The old `repos/lusoris/vmaf` endpoint
returns 404, so a live URL is not a durable source. The repository-qualified
plain-text identity remains the honest historical namespace, with the Git
objects above as executable local evidence.

The original repair covered only the state ledger, two ADRs, and three research
digests. A complete tracked-text audit found the same identities in the CUDA
source comments and AGENTS invariant, CAMBI operator docs, sync reports,
changelog fragments and their generated `CHANGELOG.md` contexts. It also found
the adjacent archived `lusoris/vmaf#866` and `lusoris/vmaf#752` identities, plus
two BVI-DVC pages that incorrectly called ADR-0310's implementation "PR 310";
commit `a159ba9da` proves the implementation actually landed as
`lusoris/vmaf#407`.

Not every matching number is historical. Research-0870's Containerfile note
still correctly says active `PR #239`: commit `b0cfd6fb` is the active fork's
Cython source-root rename and the prose explicitly describes that work. That
reference remains unchanged. This evidence-based split is why the gate is
context-scoped rather than a repository-wide number rewrite.

## Feedback loop

`scripts/ci/check-issue-reference-provenance.py` identifies one logical
Markdown context through stable prose, then requires the proven
repository-qualified `lusoris/vmaf` identities inside that context. It rejects
both a bare form and an explicit `VMAFx/vmafx` qualified identity or GitHub URL
for the same historical number. Logical blocks are whitespace normalised, so
line wrapping and paragraph reflow do not disable the check.

The checker was added and run before the documentation repair. Against the
exact collector base it exited 1 across the missed changelog, source, AGENTS,
metric, sync-report, state, and research classes. After the repair it exits 0
over 48 contexts. Its unit suite proves:

- qualified archived references pass;
- the exact bare archived references fail;
- a wrong explicit `VMAFx/vmafx` URL fails with a repository-specific finding;
- an unrelated bare active-fork PR reference outside the historical context
  remains allowed;
- whitespace reflow does not disable a contract;
- missing or duplicated context anchors fail closed; and
- empty or missing files fail closed under repository root verification.

## Decision matrix

| Option | Precision | Rebase durability | Decision |
| --- | --- | --- | --- |
| Ban every bare issue number in documentation | Low: ordinary active-fork references are valid | High, but creates broad churn and false positives | Rejected |
| Assert exact lines or occurrence counts | Medium | Low: harmless reflow breaks it; duplicated prose can evade counts | Rejected |
| Query GitHub during CI | Medium | Low: the archived endpoint is gone and network state is mutable | Rejected |
| Context-scoped repository-provenance contracts | High: only proven historical identities are governed | High: stable prose anchors, local evidence, fail-closed drift | Chosen |

## Boundaries

This slice changes prose, source comments, and its regression gate only. It does not
close BUG-048, change a Netflix golden assertion, alter a score, benchmark,
tune performance, or retrain a model. Existing `Netflix/vmaf` references and
ordinary active-fork issue references remain outside these contracts.

## Reproducer

```bash
python3 scripts/ci/check-issue-reference-provenance.py
python3 -m unittest scripts/ci/tests/test_issue_reference_provenance.py
```
