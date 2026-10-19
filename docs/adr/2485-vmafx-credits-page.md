<!-- markdownlint-disable MD013 MD060 -->
# ADR-2485: VMAFx keeps its own credits page, generated from a curated list and held to the tree

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: `docs`, `license`, `process`

## Context

VMAFx ships, vendors and adapts other people's work: Netflix VMAF itself, code
from libjxl, Xiph, IQA, libsvm, cJSON, dav1d and x264, model weights, fonts, a
chart runtime, and it learns from papers and datasets. Attribution lived in
file headers, `NOTICE`, `LICENSES/`, `REUSE.toml` and scattered model cards.
Nothing said in one place what the project owes whom, and a file installed from
the praetor engine (`.agents/skills/caveman/SKILL.md`) already points readers to a
`docs/credits.md` that does not exist. The engine offers adopters no credits
command (praetor issue 850), so VMAFx builds its own.

## Decision

1. `docs/credits.yaml` is the curated list: one entry per third-party item with
   `id`, `name`, upstream `url`, `kind`, `relation` (shipped, vendored, adapted,
   inspired, used-by-CI, integrated), `license` (an SPDX expression exactly as the
   upstream states it, or `proprietary`, `none` or `unknown`; never a guess), and
   optional `paths`, `evidence`, `license_note` and `note`.
2. `docs/credits.md` keeps its hand-written prose; its tables are rendered from the
   list by `scripts/docs/generate-credits.py` between marker comments, wired into
   `make docs-fragments-write` and `make docs-fragments-check` like the other
   generators.
3. `scripts/docs/check-credits.py` fails on: (1) page drift; (2) a vendored or
   inherited third-party path with no entry (non-project licences in `REUSE.toml`,
   `third_party`, `3rdparty` and `vendor` directories, notice and licence files,
   fonts, source headers that name a foreign copyright holder); (3) a
   `LICENSES/*.txt` licence that no entry and no project code uses; (4) a skill or
   agent file that declares an upstream (`derived_from`, "Adapted from") with no
   entry for it, which holds praetor-installed files to their credit; (5) an entry
   path or evidence file missing from the checkout.
4. An exception names one file, one rule, a reason and an expiry, lives in the
   `exceptions:` list of the credits file, and fails once expired or when it excuses
   nothing.
5. Scope: upstream and vendored code (Netflix VMAF first); adapted texts and
   inspirations; models, datasets and papers implemented clean-room, each citing in
   `evidence` the repository file that states its facts; build and CI tools,
   actions, base images and fonts.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep attribution in headers, `NOTICE` and `REUSE.toml` only | Nothing new to maintain | No single readable page; models, datasets and papers have no home; no check | The gap this ADR closes |
| Hand-written page without a list | Simple | Drifts silently; nothing ties it to the tree | Item 2 of the decision is the point of the gate |
| Generate the page from `REUSE.toml` and package manifests alone | No curation | Cannot hold relation, upstream URL, papers, datasets or adapted texts; licence strings would be machine guesses | The list must say how an item reaches the project |
| Wait for a credits command in the praetor engine | One implementation for the fleet | No date; the maintainer's page is needed now (praetor issue 850) | Built here, offered upstream |

## Consequences

- **Positive**: one page names everyone the project builds on, with the licence
  each upstream states; a new vendored directory, font or notice file fails the
  gate until credited; unknown licences are visible as a count.
- **Negative**: curation work: every direct dependency and vendored file needs an
  entry. The list names direct dependencies only; the release images carry the full
  notices.
- **Neutral / follow-ups**: entries stating `unknown` are gaps to close;
  the praetor-installed skills `adhd-format` and `social-text` are credited through
  `i-have-adhd`, and `caveman` through its own entry; a later pull request that adds
  an installed file adds its entry in the same change.

## References

- `Q-204`, `Q-205` (maintainer decisions, 2026-10-08): VMAFx builds its own credits
  page from a curated list, generated and gated, in the scope above.
- `Q-206`: the pull request that installs the praetor skills waits for this page.
- Praetor issue 850: the engine offers adopters no credits command.
- [ADR-1250](1250-eupl-fork-relicense.md) (file licences),
  [ADR-0221](0221-changelog-adr-fragment-pattern.md) (the fragment and generator
  pattern), [ADR-1428](1428-exact-twins-fragments.md).
