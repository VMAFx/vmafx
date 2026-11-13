<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 MD060 -->
# `scripts/release/` — agent invariants

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

Fork-local release utilities. Nothing here merges from upstream Netflix/vmaf.

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `build-native-release-artifacts.sh`, `tests/test-build-native-release-artifacts.sh`, `/dev/Containerfile`, `/.github/workflows/supply-chain.yml` | [build-native-release-artifacts](AGENTS.d/build-native-release-artifacts.md) | Compile inside Debian 13 release-build stage only; patchelf $ORIGIN on staged CLI copy. |
| `check-release-bot-secrets.sh`, `tests/test-check-release-bot-secrets.sh`, `/.github/workflows/release-please.yml` | [check-release-bot-secrets](AGENTS.d/check-release-bot-secrets.md) | Require RELEASE_BOT_APP_ID and RELEASE_BOT_PRIVATE_KEY; never add GITHUB_TOKEN fallback. |
| `concat-changelog-fragments.sh`, `tests/test-concat-changelog-fragments.sh`, `/changelog.d/**` | [concat-changelog-fragments](AGENTS.d/concat-changelog-fragments.md) | EXIT trap immediately after both mktemp calls; sole CHANGELOG.md owner; preserve header. |
| `distribution.py`, `lint-distribution-recipes.sh`, `tests/test_distribution.py`, `/packaging/manifest.toml` | [distribution](AGENTS.d/distribution.md) | packaging/manifest.toml = single source for Homebrew, conda-forge, winget; never hand-write. |
| `pep440-version.sh`, `tests/test-pep440-version.sh`, `/.github/workflows/supply-chain.yml` | [pep440-version](AGENTS.d/pep440-version.md) | Exact X.Y.Z / X.Y.Z-rc.N -> PEP 440 filenames without packaging; SBOM keeps SemVer. |
| `tests/test-publication-environment-binding.sh`, `/.github/workflows/supply-chain.yml`, `/.github/workflows/docker-publish-*.yml` | [publication-environment-binding](AGENTS.d/publication-environment-binding.md) | Server-side environment preflight + YAML binding check; SHA-pinned actions; exact tag identity. |
| `publish_homebrew_tap.py`, `tests/test_publish_homebrew_tap.py`, `/.github/workflows/supply-chain.yml` | [publish-homebrew-tap](AGENTS.d/publish-homebrew-tap.md) | Open signed-off tap PR from distribution.py output on final versions only; never push main. |
| `/release-please-config.json`, `/.release-please-manifest.json`, `rollover-changelog-fragments.sh`, `check-one-shot-fields.sh`, `tests/test-check-one-shot-fields.sh`, `/.github/workflows/release-please.yml`, `/.github/workflows/rule-enforcement.yml`, `/core/meson.build` | [release-line-invariants](AGENTS.d/release-line-invariants.md) | Fork starts at v0.1.0; release-as + bootstrap-sha one-shot; SONAME independent; 1 marker/file. |
| `rollover-changelog-fragments.sh`, `tests/test-rollover-changelog-fragments.sh`, `/.pre-commit-config.yaml` | [rollover-changelog-fragments](AGENTS.d/rollover-changelog-fragments.md) | Release-PR cut consumes fragments, retires one-shot fields, writes SHA-256 receipt. |
| `verify-native-release-artifacts.sh`, `tests/test-verify-native-release-artifacts.sh`, `/core/include/meson.build` | [verify-native-release-artifacts](AGENTS.d/verify-native-release-artifacts.md) | Both libvmaf + libvmafx chains staged as regular files; DT_RUNPATH=$ORIGIN under env -i. |
| `verify-release-version.sh`, `tests/test-verify-release-version.sh`, `/release-please-config.json` | [verify-release-version](AGENTS.d/verify-release-version.md) | Publication preflight verifies manifest, single extra-files markers, tag commit, rollover cut. |
