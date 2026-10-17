# Distribution manifest

Every file VMAFx ships, and every channel that carries those files, is
described once in [`packaging/manifest.toml`](../../packaging/manifest.toml).
[`scripts/release/distribution.py`](../../scripts/release/distribution.py)
writes each channel's recipe or metadata from that file, and CI fails when the
manifest disagrees with the tree. The decision record is
[ADR-3317](../adr/3317-distribution-manifest.md).

## Quick start

Check the manifest against the tree (runs in a second, no network):

```bash
python3 scripts/release/distribution.py check
```

Write every channel's metadata for a release:

```bash
python3 scripts/release/distribution.py generate \
  --version 1.0.0 \
  --source-sha256 <sha256 of v1.0.0.tar.gz> \
  --winget-sha256 <sha256 of vmafx-tester-windows-x64-v1.0.0.zip> \
  --out /path/to/output
```

The output directory gets:

| Path | Channel | Contents |
| --- | --- | --- |
| `homebrew/vmafx.rb` | Homebrew | formula building the source archive with Meson |
| `conda-forge/recipe.yaml` | conda-forge | `rattler-build` recipe (schema version 1) |
| `winget/VMAFx.VMAFX*.yaml` | winget | version, default-locale and installer manifests (schema 1.12.0) |
| `index/<channel>.json` | every channel | state, tracking issue and the files the channel carries for that version |

Run every channel's own linter on the generated files (needs Docker; pulls two
digest-pinned images):

```bash
scripts/release/lint-distribution-recipes.sh            # temporary directory
scripts/release/lint-distribution-recipes.sh --keep out # keep the files
```

The script runs `brew style` and `brew audit --strict` offline in a throwaway
tap, and `conda-smithy recipe-lint --conda-forge` in a staged-recipes layout.
Set `PYTHON` to pick the interpreter (it needs PyYAML).

## The manifest

An artifact entry says what the file is and where it comes from:

```toml
[artifacts.tester-windows-x64]
kind = "tester-bundle"
platform = "windows-x64"
abi = "none"
backends = ["cpu"]
licence = "EUPL-1.2 AND BSD-2-Clause-Patent AND ..."
licence_record = "windows-zip"
files = ["vmafx-tester-windows-x64-v{version}.zip"]
signing = "sigstore"
sbom = "vmafx-tester-windows-x64-v{version}.zip.spdx.json"
producer = { workflow = "windows-tester-bundle.yml", job = "Build and test the zip (Windows x64)" }
```

- `backends` is any of `cpu`, `cuda`, `sycl`, `hip`, `metal`; the generated
  formula and recipe pass the matching `-Denable_*` Meson options.
- `licence` is an SPDX `AND` expression. It must equal the licences of the
  record named by `licence_record` in
  [`tools/rc1-tester/image/licensing.json`](../../tools/rc1-tester/image/licensing.json),
  or the licence field of the file named by `package`
  (`pyproject.toml`, `Cargo.toml`). Every licence needs its text in
  `LICENSES/`.
- `files` may use `{version}` (`1.0.0-rc.4`) and `{pep440}` (`1.0.0rc4`).
- `producer` names the workflow and the job name that build the file, or
  `{ forge = "tag-archive" }` for the forge's source archive. An artifact no
  pipeline builds yet carries `tracking = "#<issue>"` instead.

A channel entry lists artifact keys:

```toml
[channels.winget]
state = "planned"
tracking = "#2319"
artifacts = ["tester-windows-x64"]
metadata = ["winget-manifests"]
```

`state` is `published` (the release pipeline uploads it) or `planned` (an
issue publishes it, named in `tracking`).

## What the check refuses

`distribution.py check` exits 1 and names the entry for each of:

- a channel naming an artifact the manifest does not define, or one whose
  `tracking` says no pipeline builds it yet;
- a licence that differs from its licensing record or package manifest, or
  has no text in `LICENSES/`;
- a producer workflow that does not exist, or a job name it does not define;
- an artifact with both `producer` and `tracking`, or neither;
- an unknown backend, kind or key;
- a planned channel without a tracking issue.

It exits 2 when the manifest cannot be read.

## Where it runs

- Pre-commit hook `distribution-manifest`: the check and the unit tests in
  [`scripts/release/tests/test_distribution.py`](../../scripts/release/tests/test_distribution.py),
  on any change to `packaging/`, the generators, the licensing record,
  `REUSE.toml`, the package manifests or the producer workflows.
- Required job `Release Script Contract`: the check on every pull request; the
  channel linters when the pull request touches the manifest, the generators,
  the licensing record or `REUSE.toml` (otherwise the step prints why it
  skipped).
- The winget schemas in `packaging/schemas/winget-1.12.0/` are vendored from
  microsoft/winget-cli and held to their hashes by
  `scripts/docs/check_vendored_assets.py`.

## Adding a channel or an artifact

1. Add the artifact with its `producer` (or `tracking`) and licence.
2. Add it to the channels that carry it.
3. Run `python3 scripts/release/distribution.py check` and the linter script.
4. Publishing a planned channel means changing its `state` to `published` and
   calling `generate` from the release workflow; Homebrew, conda-forge and
   winget do so under #2319, crates.io under #2320, npm under #2248 and the
   integrator pack under #2146.

## Related documents

- [Publishing artifacts](publishing.md): which pipeline builds each file.
- [Release](release.md): the release process.
- [Licence provenance check](licence-provenance-check.md).
