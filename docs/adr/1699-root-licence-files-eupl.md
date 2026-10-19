<!-- markdownlint-disable MD013 MD060 -->
# ADR-1699: The root licence files state ADR-1250's terms, and every package manifest declares the licences of the files it ships

- **Status**: Accepted (Helm chart row amended by [ADR-2673](2673-chart-licence-kubernetes-schemas.md))
- **Date**: 2026-10-05
- **Deciders**: maintainer, agent
- **Tags**: license, compliance, docs, ci, rust, helm, python, go, fork-local

## Context

[ADR-1250](1250-eupl-fork-relicense.md) licensed fork-authored code under
EUPL-1.2, left code that carries Netflix's or another project's work on its
terms, and made each file's SPDX header the authority. It dropped the MIT
alternative of 123 Go files on purpose: an MIT option lets a downstream take
the permissive branch and defeat the reciprocity the decision exists for.

The repository root did not follow. It held `LICENSE`, Netflix's
BSD-2-Clause-Patent text ("Copyright (c) 2020 Netflix, Inc."), and
`LICENSE-MIT` ("Copyright (c) 2026 Lusoris"), which
[ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md) added when it
proposed dual-licensing fork code under the Netflix licence or MIT. The EUPL-1.2
text existed only as `LICENSES/EUPL-1.2.txt`. GitHub's licence detector
(licensee) matched `LICENSE-MIT` exactly and `LICENSE` not at all, so the
repository sidebar reported "MIT licenses found" and the repository API reports
`NOASSERTION`. No licence check read the root.

Package manifests restated the same stale terms. Every Rust source file is
EUPL-1.2, yet the workspace `Cargo.toml` (inherited by `vmafx-sys` and
`vmafx-tad`) and `bindings/rust/vmafx/Cargo.toml` declared
`BSD-2-Clause-Patent`; the Helm chart's `artifacthub.io/license` did too, for
a chart whose files are all EUPL-1.2; `tools/rc1-tester` declared `EUPL-1.2`
while its sdist carries two MIT third-party notices. `REUSE.toml` recorded the
vendored Prometheus Pushgateway chart archive under the fork's EUPL-1.2 default,
although that archive's own `Chart.yaml` declares Apache-2.0.
[ADR-1560](1560-python-package-licence-union.md) already held seven Python
packages to the files they ship; nothing held the others. The `.toml`
manifests themselves were outside the provenance tool of ADR-1250: it skipped
every file type it could not comment, so 13 fork `.toml` files had no header
and two fork `pyproject.toml` files still carried `BSD-2-Clause-Patent`.

[Research-2143](../research/2143-root-licence-files-and-package-manifests.md)
records the measurements and which published artifacts contained `LICENSE-MIT`.

## Decision

The repository root holds one licence file and Netflix's notice, as part of
ADR-1250's policy:

- `LICENSE` is the EUPL-1.2, byte for byte `LICENSES/EUPL-1.2.txt`, the licence
  of fork-authored code. licensee 10.1.0 identifies it as `EUPL-1.2` (Dice
  similarity 99.20 %, threshold 98 %).
- `NOTICE` is the former `LICENSE`, unchanged: Netflix's BSD-2-Clause-Patent
  text with its copyright notice, moved with `git mv`. licensee does not score
  a file of this name as a licence file, so `LICENSE` is the only root licence
  file. licensee 9.18.0, which reads the root only, reports the project as
  `EUPL-1.2` (with the Netflix text under a licence-file name, first
  `LICENSE-BSD-2-Clause-Patent` on this branch, it reported `NOASSERTION`).
  licensee 9.19.0 and later also read `LICENSES/`, whose 15 texts make it
  report `NOASSERTION` for this tree, as it does for `eza-community/eza`, which
  GitHub reports as `eupl-1.2`. GitHub's licence API returns `eupl-1.2` for
  this layout (`?ref=` of the pull-request branch, 2026-10-05) and returned
  `other` for the `LICENSE-BSD-2-Clause-Patent` revision.
  `REUSE.toml` annotates `NOTICE` with its own terms (BSD-2-Clause-Patent,
  2020 Netflix). The artifact gates of
  [ADR-1503](1503-tester-artifact-licensing.md) and
  [ADR-1513](1513-production-artifact-licensing.md) take the
  BSD-2-Clause-Patent text from it (`licensing.json` `spdx_texts`), so every
  shipped artifact keeps carrying the same bytes.
- `LICENSE-MIT` is removed. This supersedes the `LICENSE-MIT` line of
  ADR-0686, which is Proposed; ADR-1250 had already replaced its dual-licence
  policy.

Per-file SPDX headers stay authoritative and `LICENSES/` is unchanged. The
README's licence section names which code carries which licence and points to
the headers.

The two tags whose tree held `LICENSE-MIT` and that the Go module proxy
serves, `v1.0.0-rc.1` and `v1.0.0-rc.2`, are retracted in the root `go.mod`
(`retract [v1.0.0-rc.1, v1.0.0-rc.2]`, rationale "contained a stale root
LICENSE-MIT; licensing is per-file SPDX ..."). The tags stay. The licensing page
and the release notes of the next candidate state the same facts: the file was
a leftover of ADR-0686, ADR-1250 governs, the per-file headers decide.

Every package manifest that declares a licence declares the SPDX identifiers of
exactly the files its package ships (an AND expression; an OR is refused for the
reason ADR-1250 gives): `pyproject.toml` (ADR-1560's model), `Cargo.toml` (the
tracked files of the crate outside nested packages, the workspace `Cargo.lock`,
the workspace manifest whose fields it inherits, and a readme copied from
outside, as `cargo package --list` reports them), Helm `Chart.yaml` (the chart's
own files; a subchart archive is a package of its own whose `Chart.yaml`
declaration must equal its `REUSE.toml` record) and `package.json` (a manifest
without a licence must be `private`). A configuration the model does not
describe (`include`, `exclude`, `license-file`, `.helmignore`, `files`) fails
rather than being guessed. The manifests now declare:

| Package | Field | Value |
| --- | --- | --- |
| `vmafx-sys`, `vmafx` | `license` | `EUPL-1.2` |
| `vmafx-tad` (not published) | `license` | `EUPL-1.2 AND BSD-2-Clause-Patent` (it ships the root `README.md`, BSD-2-Clause-Patent in `REUSE.toml`) |
| Helm chart `vmafx` | `artifacthub.io/license` | `EUPL-1.2` |
| `vmafx-rc1-tester` | `license` | `EUPL-1.2 AND MIT`, with `license-files` |

`scripts/ci/check_licence_metadata.py` enforces the root layout and the manifest
rule. It refuses a second root licence file (every name licensee 10.1.0 scores:
`LICENSE*`, `COPYING*`, `COPYRIGHT*`, `OFL*`, `PATENTS*`, `<x>-LICENSE*`), a
`LICENSE` that is not the EUPL-1.2 text, a missing `NOTICE` and a `NOTICE`
without Netflix's copyright line. It runs in the required `Licence Provenance`
job, on every commit (pre-commit hook `check-licence-metadata`), and its tests
run in the tooling suite, which root licence changes now select.

`scripts/dev/relicense_fork_files.py` classifies `.toml` files like the other
commentable types, so the `Licence Provenance` job holds a fork manifest or
configuration file to an EUPL-1.2 header. `pyproject.toml` joins the names with
no provenance signal (ADR-1250 veto 3): upstream's `python/pyproject.toml` does
not make a fork package's manifest Netflix's. praetor's Codex projections under
`.codex/agents/` are not candidates; the engine writes them from
`.agents/agents/`. The dev image states the licences of the VMAFx files its
published stage copies, not `BSD-2-Clause-Patent`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| EUPL-1.2 as `LICENSE`, Netflix's text as `NOTICE`, `LICENSE-MIT` removed (chosen) | `LICENSE` is the only root licence file, and a root-only detector reports EUPL-1.2; Netflix's notice stays at the root, where its licence asks for it in source distributions; artifact texts unchanged | `NOTICE` is a convention name, not a licence-file name; a reader has to follow the README to find Netflix's terms | The maintainer's decision (second popup); the only layout of the three where a root-only detector reports one project licence |
| EUPL-1.2 as `LICENSE`, Netflix's text as `LICENSE-BSD-2-Clause-Patent` | The file name states the licence | licensee scores two licence files with different licences and reports `NOASSERTION` (measured) | The first revision of this ADR; replaced after the measurement |
| An overview file at the root (`LICENSE` explaining the per-file rule, texts under `LICENSES/`) | One file states the whole rule | licensee matches no licence in prose, so GitHub reports nothing; an explanation is not a licence text and duplicates the README | The EUPL-1.2 text must be detectable as such |
| Only remove `LICENSE-MIT` | Smallest change | The root would still present BSD-2-Clause-Patent as the project licence while 1,514 fork files are EUPL-1.2 | Leaves the contradiction ADR-1250 created half-fixed |
| Leave rc.1 and rc.2 unretracted, or delete their tags | No go.mod change; deletion removes the archives GitHub builds | Unretracted, nothing in the module records that the file was stale; deleting tags breaks every consumer of the release candidates and does not remove the proxy's copies | The maintainer chose "Retract + state it": tags stay |
| Declare `EUPL-1.2 AND Apache-2.0` for the Helm chart, counting the subchart archive | One number for everything `helm package` puts in the archive | Artifact Hub requires a single SPDX identifier in `artifacthub.io/license`; the subchart is a chart with its own declared licence | Each chart declares its own files; the subchart's record is checked against its declaration |
| Hold manifests only to "includes every licence its files carry" | Tolerates an over-broad declaration | A manifest that keeps `BSD-2-Clause-Patent` next to `EUPL-1.2` for EUPL-only files still misstates the terms | ADR-1560 already requires equality for Python packages; one rule for all manifests |
| Exempt `.toml` from the provenance tool with a dated exception | No header churn | 13 fork files without a header, and the next manifest arrives without one too | A header is one comment line in TOML; the tool already writes it |

## Consequences

- **Positive**: GitHub, scanners and readers find the EUPL-1.2 as the only root
  licence file (licensee 9.18.0 reports `EUPL-1.2` for the project); no root file offers a permissive
  branch for fork code; every manifest restates what the files say, and a new
  file with another licence fails the commit that adds it to a package.
- **Negative**: an upstream sync that touches Netflix's `LICENSE` must land the
  change in `NOTICE` ([rebase notes](../rebase-notes.md)). The `go.mod`
  retraction is not read by Go clients while this module path's latest version
  is Netflix's tag `v3.0.0+incompatible`: the go command loads retractions from
  the `go.mod` of `@latest`, and that version has none (measured with a local
  module proxy: with rc.3 carrying the directive, `go list -m -versions` still
  lists rc.1 and rc.2; only a release above `v1.5.3` with the directive makes
  `@latest` resolve to it). History still follows the move: `git log --follow
  NOTICE` lists the old `LICENSE` commits after a squash that also rewrites
  `LICENSE` (checked on a throwaway repository).
- **Neutral / follow-ups**: `LICENSE-MIT` remains in published artifacts built
  before this change (Research-2143: the Go module proxy's zips of
  `v1.0.0-rc.1` and `v1.0.0-rc.2` and GitHub's source archives of tags from
  2026-05-28 on). The crates ship no licence text; none has been published to
  crates.io. The dev image label is not yet derived by a check. Should GitHub
  move to a licensee that reads `LICENSES/` (9.19.0 or later), the repository
  field would read `NOASSERTION` again, as it would for every REUSE-layout
  repository with more than one licence text.

## References

- [ADR-1250](1250-eupl-fork-relicense.md), [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md),
  [ADR-1503](1503-tester-artifact-licensing.md), [ADR-1513](1513-production-artifact-licensing.md),
  [ADR-1560](1560-python-package-licence-union.md), [ADR-1564](1564-dev-image-private-guard.md),
  [Research-2143](../research/2143-root-licence-files-and-package-manifests.md).
- licensee 10.1.0 (`gem install licensee`, `licensee detect`), run 2026-10-05;
  its `Licensee::ProjectFiles::LicenseFile::FILENAME_REGEXES`.
- Go modules reference, `retract` directive and `@latest` resolution
  (<https://go.dev/ref/mod>), read 2026-10-05; go 1.27.1.
- Artifact Hub `docs/helm_annotations.md`: `artifacthub.io/license` "must be a
  valid SPDX identifier", read 2026-10-05.
- req: "i guess tags and description need an update as well" (with a screenshot
  of the repository sidebar showing "License, MIT licenses found").
- Q (popup, 2026-10-05): "EUPL at root, Netflix kept (Recommended)".
- Q (popup, 2026-10-05): "Rename to NOTICE (Recommended)".
- Q (popup, 2026-10-05): "Retract + state it (Recommended)".
