<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3317: One distribution manifest describes every artifact and channel

- **Status**: Accepted
- **Date**: 2026-10-15
- **Deciders**: Lusoris (VXM1 milestone order steering-20261015-wave3, issue #2314)
- **Tags**: release, packaging, licence, ci, supply-chain

## Context

VMAFx ships native Linux files, model archives, a Python wheel and sdist, and
tester bundles for Windows and macOS, and plans Homebrew, conda-forge, winget,
crates.io, npm and an integrator pack (#2319, #2320, #2248, #2146). Each
channel needs the same facts: file names, platform, ABI, backends, licence,
signing and SBOM. Written per channel, those facts drift: a recipe names a
file no pipeline builds, or a licence field disagrees with the licensing
record in `tools/rc1-tester/image/licensing.json` that the release audit
holds the artifacts to. Issue #2314 asks for one source, generators for every
channel, a drift check that fails CI, and fixture output that passes each
channel's own linter.

## Decision

`packaging/manifest.toml` describes every artifact once and every channel by
the artifact keys it carries. `scripts/release/distribution.py check` fails
when the manifest disagrees with the tree: a licence that is not the union of
its licensing record's components or not its package manifest's licence field
(the same reader as `scripts/ci/check_licence_metadata.py`), a licence
without its text in `LICENSES/`, a producer job its workflow does not define,
a channel naming an artifact no pipeline builds, or a planned channel without
a tracking issue. `distribution.py generate` writes the Homebrew formula, the
conda-forge recipe, the three winget manifests and one JSON index per channel
for a release version. The check runs in the required `Release Script
Contract` job and as a pre-commit hook; the same job runs
`scripts/release/lint-distribution-recipes.sh` (brew style and audit
`--strict`, conda-smithy `recipe-lint --conda-forge`, in digest-pinned
images) when a pull request touches the manifest or its generators. The
winget manifests are validated against the winget-cli 1.12.0 schemas
vendored in `packaging/schemas/winget-1.12.0/` and held to their recorded
hashes by `scripts/docs/check_vendored_assets.py`.

The manifest lives in `packaging/`: `dist/` is ignored by Git
(`.gitignore`) and `pkg/` is the Go module tree.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One TOML manifest + Python generators (chosen) | One reader shared with the licence check; stdlib `tomllib`; generators tested offline | Channel recipe forms re-implemented in Python | — |
| Hand-written recipe per channel + a cross-check | Recipes read as each ecosystem expects | Every fact written N times; the check must parse N formats | Drift is the problem #2314 names |
| A packaging tool (GoReleaser, cargo-dist) as the source | Upstream maintains the channel templates | Neither covers conda-forge, winget and the tester bundles together; a second configuration format beside the licensing record | One behaviour, one implementation (HISS-19) |
| Manifest under `dist/` | Name matches the topic | Ignored by Git | Cannot be tracked |

## Consequences

- **Positive**: a channel, a file name or a licence changes in one place; CI
  fails on an artifact a channel names but no pipeline builds, and on a
  licence that differs from the licensing record or package manifest.
- **Negative**: the `Release Script Contract` job takes longer when the
  linters run (two container pulls); its timeout rises from 5 to 25 minutes.
- **Neutral / follow-ups**: publishing to Homebrew, conda-forge and winget
  stays with #2319, crates.io with #2320, npm with #2248, the integrator pack
  with #2146; each switches its channel to `published` and wires `generate`
  into the release workflow. macOS and Windows channels take their files from
  the attested native pipelines (ADR-2383).

## Supply-chain impact

- **New dependencies**: none at runtime. Test: `jsonschema` (already in the
  tooling-tests lock). CI: `pyyaml` from `requirements/locks/build.txt`
  installed with `--require-hashes`; images `ghcr.io/homebrew/brew` and
  `condaforge/miniforge3` pinned by digest; `conda-smithy` 3.62.0.
- **Vendored**: three winget-cli 1.12.0 schema files and its licence (MIT),
  annotated in `REUSE.toml`.

## References

- Issue #2314; parent #2437.
- Operator order `steering-20261015-wave3` (VXM1 milestone, worked top down).
- [ADR-2383](2383-package-channels-native-pipelines.md) for the native
  macOS and Windows package-channel pipelines.
- Research: no digest needed; the channel rules come from the linters
  themselves (brew audit, conda-smithy, the winget schemas) run on the
  generated output.
