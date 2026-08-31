<!-- markdownlint-disable MD013 -->
# Research-1127: Single SemVer release stream

## Question

What is the smallest safe migration from the inherited fork suffix and four
release-please packages to one ordinary patch-release stream?

## Evidence

- The source and root manifest identify 3.2.0 as the current baseline; remote
  tags stop at `v3.0.0`, and the repository has no published GitHub release.
- The component packages used unqualified tags, so independently generated
  component releases could collide and all could invoke whole-product release
  automation.
- GitHub suppresses new workflow runs for most events created with the
  repository `GITHUB_TOKEN`. Keeping release-please's draft release and having
  an authenticated operator publish it provides a reliable tag and
  `release.published` fan-out without adding a privileged token. A preflight
  pauses both release-please action phases while an ordinary-SemVer draft exists,
  so a later master push cannot duplicate or retarget the pending release.
- Release-please 17.6.0 and 17.11.2 simulations both selected 3.2.1 through the
  one-time `release-as` field, including when the complete migration tree was
  represented as a single squash commit directly above the bootstrap SHA. With
  a 3.2.1 tag and manifest, retaining that override generated another invalid
  3.2.1 cut, while deleting it generated 3.2.2. The release-PR rollover therefore
  validates and removes `release-as` together with `bootstrap-sha` before the
  release PR merges.
- A normal development checkout also contains Netflix upstream's local
  `v3.1.0` and `v3.2.0` tags, although `VMAFx/vmafx` advertises neither tag.
  Release-please local mode will treat those shared-namespace tags as fork
  history and can mask `bootstrap-sha`. Repeating both version simulations in
  an origin-faithful clone with only the fork's actual tags selected the
  configured `98dc0b2b...` bootstrap and the same single 3.2.1 release PR.
- Helm chart `version` and Rust crate versions describe independently packaged
  artifacts. Helm `appVersion`, the Python distributions, libvmaf, and the node
  image describe the coordinated VMAFx release and should move together.
- The compatibility `vmaf` distribution was still dynamically reading
  `compat/python-vmaf/__init__.py` as `3.0.0`, even though libvmaf and the
  fork-local Python packages had reached `3.2.1`. Making that file a
  release-please marker removes the last public-package version split while
  retaining the independent SONAME/API version in `core/meson.build`.
- A clean 1,264-target Meson build places the shared-library chain under
  `build/src/`; the release workflow's former `build/libvmaf/` copy path did
  not exist and would have stopped publication before signing. Staging only a
  dereferenced `libvmaf.so` was also insufficient: the built CLI declares
  `DT_NEEDED libvmaf.so.3`, and GitHub artifact downloads do not preserve
  symlinks. The repaired stage materializes the unversioned, SONAME, and real
  names as identical regular files. A verifier parses the ELF SONAME and
  `DT_NEEDED`, proves `ldd` resolves the downloaded SONAME file under `env -i`,
  and runs the downloaded CLI for the exact release version before native
  signing or provenance. The model archive now sorts entries, normalizes
  ownership and mtimes to the tag commit, and suppresses the gzip timestamp so
  recovery does not change that asset solely because it ran later.
- The `vmaf-mcp` project is not yet present on PyPI (the JSON project endpoint
  returned HTTP 404 on 2026-08-31). Its first publication therefore depends on
  a Pending Trusted Publisher for `VMAFx/vmafx`, `supply-chain.yml`, and the
  `pypi-publish` environment; ADR-0166 originally recorded the pre-transfer
  identity. The matching GitHub environment and exact PyPI Pending Trusted
  Publisher were both created on 2026-08-31. PyPI now shows the pending row for
  `vmaf-mcp` / `VMAFx/vmafx` / `supply-chain.yml` / `pypi-publish`.
- Release hashing and signing iterated over unguarded bare `*` pathnames. The
  repaired loops use an option terminator (or `find` output rooted at `./`) so a
  dash-prefixed artifact cannot become an option while SLSA subjects retain
  their release filenames.
- The release workflow produced hashes for the MCP wheel and sdist without
  consuming them in a provenance job, and generated SBOMs before downloading
  those distributions. The repaired DAG creates distinct SLSA subjects and
  names for libvmaf and `vmaf-mcp`, scans every native artifact plus an installed
  Python runtime dependency closure in SPDX and CycloneDX formats, validates
  package identity, dependency edges, and exact artifact SHA-256 values, signs
  the SBOMs, and refuses incomplete attachment globs. Syft 1.51.1 was used for
  the validated inventory shape.
- Manual recovery now requires the workflow itself to run at the existing
  published `vX.Y.Z` tag, not merely check that tag out in a master-context run.
  This keeps the SLSA event SHA/ref identical to the source being attested and
  preserves the authenticated publication gate. PyPI recovery compares the
  exact two-filename set and SHA-256 digests before and after `skip-existing`,
  so a same-version rebuild or partial-upload race cannot silently diverge from
  the index. The package build frontend and hatchling backend are pinned to the
  live PyPI releases verified on 2026-08-31 (`build==1.6.0`,
  `hatchling==1.32.0`). A local isolated build produced exactly
  `vmaf_mcp-3.2.1-py3-none-any.whl` and `vmaf_mcp-3.2.1.tar.gz`, with wheel
  metadata `Name: vmaf-mcp`, `Version: 3.2.1`, and `Requires-Python: >=3.10`.
  Repeating that build with a fixed tag-commit `SOURCE_DATE_EPOCH` and
  `PYTHONHASHSEED=0` produced byte-identical wheel and sdist hashes, making the
  filename/hash recovery guard usable after partial publication.
- The two Docker recovery workflows still accepted an independent image-tag
  input while checking out the dispatch ref. A run from `master` with input
  `v3.2.1` could therefore sign and publish non-tag source under the release
  name, and GitHub-native provenance would describe the dispatch SHA rather
  than the checked-out tag. Both workflows now gate every build behind the
  same published-release, ordinary-tag, ref, SHA, checkout, and coordinated
  version equality used by the artifact workflow. Manual recovery must be
  dispatched with `--ref vX.Y.Z -f tag=vX.Y.Z`.

## Result

Use a single root release-please package with manifest baseline 3.2.0 and force
the cutover release to 3.2.1 with a one-time `release-as` override. Update all
coordinated version surfaces from the root package, retain the draft publication
gate, and have release-PR rollover remove that override and the bootstrap SHA
before merge. Do not synthesize a 3.2.0 tag.

## Reproducer

```bash
jq -e '.packages | keys == ["."]' release-please-config.json
jq -e '. == {".": "3.2.0"}' .release-please-manifest.json
git tag --list 'v3.2.*'
git ls-remote --tags origin 'v3.2.*'
meson setup build core -Denable_metal=disabled && meson compile -C build
test -e build/src/libvmaf.so
mkdir -p artifacts
find build/src -maxdepth 1 \( -type f -o -type l \) \
  -name 'libvmaf.so*' -exec cp -L -t artifacts -- {} +
cp build/tools/vmaf artifacts/
scripts/release/verify-native-release-artifacts.sh artifacts 3.2.1
```

## Sources

- [release-please action documentation](https://github.com/googleapis/release-please-action)
- [GitHub Actions workflow-trigger documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow)
- [SLSA generic generator documentation](https://github.com/slsa-framework/slsa-github-generator/tree/v2.1.0/internal/builders/generic)
- [Anchore SBOM action documentation](https://github.com/anchore/sbom-action/tree/v0.24.2)
- [PyPA trusted-publishing action documentation](https://github.com/pypa/gh-action-pypi-publish/tree/v1.14.2)
- [PyPI `build` metadata](https://pypi.org/pypi/build/json)
- [PyPI `hatchling` metadata](https://pypi.org/pypi/hatchling/json)
