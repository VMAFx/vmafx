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
  `release.published` fan-out without adding a privileged token.
- Helm chart `version` and Rust crate versions describe independently packaged
  artifacts. Helm `appVersion`, the Python distributions, libvmaf, and the node
  image describe the coordinated VMAFx release and should move together.

## Result

Use a single root release-please package with manifest baseline 3.2.0 and force
the cutover release to 3.2.1. Update all coordinated version surfaces from the
root package, retain the draft publication gate, and remove the bootstrap SHA
after the first release. Do not synthesize a 3.2.0 tag.

## Reproducer

```bash
jq -e '.packages | keys == ["."]' release-please-config.json
jq -e '. == {".": "3.2.0"}' .release-please-manifest.json
git tag --list 'v3.2.*'
```

## Sources

- [release-please action documentation](https://github.com/googleapis/release-please-action)
- [GitHub Actions workflow-trigger documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow)
