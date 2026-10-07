---
paths:
  - scripts/ci/check-vcs-version-not-bare-sha.sh
  - core/include/meson.build
  - scripts/ci/tests/test-check-vcs-version-not-bare-sha.sh
  - .github/workflows/macos-tester-bundle.yml
  - .github/workflows/docker-publish-tester.yml
invariant: `vcs_tag` and tester `git describe`: `--match 'v*.*.*'`, no `--always`; `fallback:`; `build.yml` `fetch-depth: 0`.
---
<!-- markdownlint-disable MD013 MD060 -->
# `check-vcs-version-not-bare-sha.sh` invariants

`core/include/meson.build` builds `VMAF_VERSION` from `git describe`, and
upstream Netflix/vmaf spells that call with `--always`. Fork deliberately
does not. With `--always`, git exits 0 even with no reachable `v*.*.*` tag,
prints bare abbreviated object name. Meson writes that into
`vcs_version.h` verbatim — so `vmaf --version`, JSON/XML `version` field
and `vmaf_version()` all report commit instead of version, on any shallow
checkout, tarball export, or worktree whose `.git` is file.

Three properties load-bearing; this gate enforces each:

| Property | Why it matters |
| --- | --- |
| No `--always` in `vcs_tag` command | It is what suppresses non-zero exit that fallback path depends on. |
| explicit `fallback:` | Meson would default it to `meson.project_version()`, but fallback *is* tagless path here; spelling it out keeps intent across meson upgrades. |
| `--match 'v*.*.*'` retained | Without it any tag in repository can supply version. |

Two things make defect easy to reintroduce, hard to notice. Conflicts
with upstream on every sync, so mechanical "take theirs" resolution restores
`--always`. Invisible until seven-character abbreviation happens
to contain no ASCII digit — about one commit in thousand — only
condition `core/test/test_output.c::test_vmaf_version` can detect. Assume any
version-string failure on one leg environmental until checked
whether checkout could reach tag.

`.github/workflows/build.yml` must therefore keep `fetch-depth: 0` on its
checkout: `git describe --long` needs both tag objects and commit
distance to them; `actions/checkout` default of 1 supplies neither.

two tester publishing workflows (`macos-tester-bundle.yml`,
`docker-publish-tester.yml`) take version of published file and image
from `git describe` as well, and gate holds each of their describes to
`--match 'v*.*.*'` and no `--always`. Without `--match`,
`tester-<date>-<sha8>` prerelease tags (made on master commits) supply
version. They omit `--long` deliberately: tagged commit then yields bare
tag (`v1.0.0-rc.2`, image `v1.0.0-rc.2-tester`), where `--long` would give
`v1.0.0-rc.2-0-g<sha>`. `scripts/ci/tests/test-check-vcs-version-not-bare-sha.sh`
plants each defect against fixture workflows.
