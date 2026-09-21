<!-- markdownlint-disable MD013 MD060 -->
# ADR-1277: Separate private state, corpora, and tracked evidence

- **Status**: Accepted
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Tags**: workspace, datasets, agents, ci, docs

## Context

The retired numbered workspace directory accumulated three unrelated classes
of data: agent state and evidence, hundreds of gigabytes of media corpora, and
rebuildable scratch output. Tracked code and public documentation still named
that private directory after the live state tooling had moved elsewhere. Some
public Markdown pages even linked into ignored files, so the published link
could never resolve in another checkout.

Treating this as a textual rename would preserve the original design error. A
single replacement directory would still mix disposable caches, reusable
datasets, local continuity records, and evidence that readers expect to find
in Git.

## Decision

The repository uses three distinct authorities:

1. `.workingdir/` is ignored, machine-local session state. It may contain the
   current OPEN/BACKLOG/BUGS ledgers, run evidence, recovery material, and
   bounded scratch/cache output. Public Markdown may show it as a command-line
   path but may not hyperlink into it or treat it as repository authority.
2. `.corpus/` is ignored, machine-local dataset storage. Raw media, extracted
   clips, derived feature tables, reusable encodes, and aggregated corpus
   JSONL files belong here. Runtime defaults and operator examples use this
   root rather than the state directory.
3. `docs/`, ADRs, changelog fragments, and source-controlled manifests are the
   durable authority for claims presented to repository readers. A public
   statement that needs evidence links to one of these tracked artifacts.

The retired numbered directory is not ignored and is not a supported runtime
path. Active code, configuration, tests, and operator documentation may not
refer to it. Historical ADRs and changelog records keep historically accurate
plain-text references, but no Markdown link may target either ignored local
root. A required repository contract check enforces these boundaries.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Blindly rename every old path to `.workingdir/` | Mechanically simple; zero old spellings in a grep | Recreates the mixed state/corpus/cache tree and rewrites historical records inaccurately | This was the incorrect first migration attempt |
| Keep both workspace roots | No migration work | Freshness remains ambiguous and stale tools silently recreate the old tree | Preserves the defect |
| Put all local data below `.corpus/` | One large-data root | Agent state, recovery bundles, and small diagnostic evidence are not datasets | Conflates lifecycle and authority |
| Track the entire local dossier | Public links resolve | Commits machine-specific state, large artifacts, and potentially licensed datasets | Violates repository hygiene and data-license boundaries |
| Split by role and enforce the boundary (chosen) | Clear lifecycle, smaller agent context, valid public evidence | Requires a one-time path migration and targeted historical-link cleanup | Only option aligned with the actual data classes |

## Consequences

- **Positive**: dataset tools agree on `.corpus/`; state tools agree on
  `.workingdir/`; public evidence resolves from a fresh clone; stale recreation
  of the retired root is visible to Git.
- **Negative**: private scripts that used the retired path need a one-time
  update, and large local data is not moved automatically by repository code.
- **Neutral / follow-ups**: local cleanup remains recoverable through a
  checksummed archive outside tracked Git. Historical text can name the old
  layout when that fact matters, but cannot link to it as live evidence.

## References

- [Research 1277](../research/1277-workingdir-contract-cleanup.md)
- Supersedes [ADR-0003](0003-workingdir2-empty-planning-dir.md) and
  [ADR-0019](0019-workingdir2-full-dossier.md).
- Source: `req` (user: "we still need to cleanout the .workingdir2 in general")
- Source: `req` (user: "well then its exactly wrong lol")
