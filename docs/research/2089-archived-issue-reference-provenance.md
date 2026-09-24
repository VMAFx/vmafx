<!-- markdownlint-disable MD013 -->
# Research-2089: Archived issue-reference provenance after the repository migration

- **Date**: 2026-09-24
- **Status**: Active
- **Scope**: BUG-048 Section E documentation provenance only
- **Base**: `4e6916d16ac57647105d14a47a6680117d6b5738`

## Question

Which bare issue and pull-request numbers in the current tree describe the
pre-migration `lusoris/vmaf` tracker, and how can the repository prevent those
identities from silently becoming links in the newer `VMAFx/vmafx` number
space without banning ordinary fork-local `#NNN` references?

## Repository evidence

Commit `2581c7ba0` qualified five established historical identities in
`docs/state.md`:

| Historical identity | Recorded subject | Resolving history |
| --- | --- | --- |
| `lusoris/vmaf#239` | FFmpeg `libvmaf_vulkan` synchronous-fence serialisation | `e266bf8e` (`lusoris/vmaf#241`) |
| `lusoris/vmaf#310` | ADR-number collision sweep cited by the Vulkan row | `af227b026` |
| `lusoris/vmaf#857` | `cambi_cuda` invalid kernel parameter / SIGSEGV | `37fcfea68` and follow-up `8de50984a` |
| `lusoris/vmaf#870` | CUDA CAMBI host-preprocessing correction | `8de50984a` |

The unrelated vendored-test commit `d468c5bb4` later restored the prior bare
spellings in `docs/state.md`; it did not explain or supersede the provenance
decision. `git show 2581c7ba0 -- docs/state.md` and
`git show d468c5bb4 -- docs/state.md` are a direct add/remove pair.

The active repository now reuses all five numbers for unrelated pull requests.
The GitHub API on 2026-09-24 identifies, for example, active `VMAFx/vmafx#239`
as the Cython source-root rename, `#241` as the legacy-runner test sunset,
`#310` as Go context propagation, `#857` as a later medium/low fix bundle, and
`#870` as a later Python CodeQL cleanup. The old `repos/lusoris/vmaf` endpoint
returns 404, so a live URL is not a durable source. The repository-qualified
plain-text identity remains the honest historical namespace, with the Git
objects above as executable local evidence.

The same archived identities survived bare in pages directly coupled to those
ledger records: ADR-0251 and Research-0042 for `#239`, the RC16 audit in
Research-0090, and ADR-0464 / Research-0135 for `#857` and `#870`. They are in
scope because a reader follows them to establish the same historical bug and
fix, not because every old-looking number in `docs/` should be rewritten.

## Feedback loop

`scripts/ci/check-issue-reference-provenance.py` identifies one logical
Markdown context through stable prose, then requires the proven
`lusoris/vmaf#NNN` identities inside that context. It also rejects a bare form
of those same numbers inside that context. Logical blocks are whitespace
normalised, so line wrapping and paragraph reflow do not disable the check.

The checker was added and run before the documentation repair. Against the
exact base it exited 1 and named every missing qualifier in the state ledger,
the two ADRs, and the three coupled research digests. After the repair it exits
0 over 15 contexts. Its unit suite proves:

- qualified archived references pass;
- the exact bare archived references fail;
- an unrelated active-fork `PR #857` outside the historical context remains
  allowed;
- whitespace reflow does not disable a contract;
- missing or duplicated context anchors fail closed; and
- empty or missing files fail closed under repository root verification.

## Decision matrix

| Option | Precision | Rebase durability | Decision |
| --- | --- | --- | --- |
| Ban every bare `#NNN` in documentation | Low: ordinary active-fork references are valid | High, but creates broad churn and false positives | Rejected |
| Assert exact lines or occurrence counts | Medium | Low: harmless reflow breaks it; duplicated prose can evade counts | Rejected |
| Query GitHub during CI | Medium | Low: the archived endpoint is gone and network state is mutable | Rejected |
| Context-scoped repository-provenance contracts | High: only proven historical identities are governed | High: stable prose anchors, local evidence, fail-closed drift | Chosen |

## Boundaries

This slice changes documentation and its regression gate only. It does not
close BUG-048, change a Netflix golden assertion, alter a score, benchmark,
tune performance, or retrain a model. Existing `Netflix#NNN` references and
ordinary active-fork `#NNN` references remain outside these contracts.

## Reproducer

```bash
python3 scripts/ci/check-issue-reference-provenance.py
python3 -m unittest scripts/ci/tests/test_issue_reference_provenance.py
```
