<!-- markdownlint-disable MD013 MD060 -->
# Inherited Netflix tags (removed 2026-10-05)

VMAFx/vmafx was created from a copy of Netflix/vmaf and carried 26 of its
release tags. [ADR-1805](../adr/1805-delete-inherited-netflix-tags.md) deleted
them from `VMAFx/vmafx` on 2026-10-05, after the maintainer decided so. The
commits stay in history and `Netflix/vmaf` keeps its own tags. This page
records what was removed and how to recreate a tag. The machine-readable copy
is [`scripts/release/inherited-upstream-tags.json`](https://github.com/VMAFx/vmafx/blob/master/scripts/release/inherited-upstream-tags.json);
the push guard and the tests read it.

## Why

- `go get github.com/VMAFx/vmafx@latest` resolved to Netflix's
  `v3.0.0+incompatible`, a tree without `go.mod`.
- Netflix's `v1.5.3` outranked every fork `v1.0.x` release, so a `retract` in
  the fork's `go.mod` never took effect.
- The fork's own stream (`v1.0.2`, `v1.1.x`, ...) would collide with the
  existing names: a GitHub release for `v1.0.2` attaches to the commit of the
  tag that already exists.

Details and the measured Go behaviour: [Release guide, Go consumers](release.md#go-consumers-and-the-inherited-tags).

## Update 2026-10-12: rc.3 and the 0.x restart

Since this record, `v1.0.0-rc.3` and four more `tester-*` pre-releases were
published (`git ls-remote --tags origin` lists 11 tags on 2026-10-12). The
release line restarts at `v0.1.0`
([ADR-3292](../adr/3292-release-line-restart-0x.md)), whose `go.mod` retracts
`v1.0.0-rc.1` to `v1.0.0-rc.3`. Measured on 2026-10-12, `proxy.golang.org`
still caches Netflix's versions up to `v3.0.0+incompatible`. Go reads
retractions from the go.mod of the highest version, so through the public
proxy `@latest` stays `v3.0.0+incompatible` and the retraction has no effect.
Pin `github.com/VMAFx/vmafx@v0.1.0` (or a later `v0.x`) explicitly.

## What was removed

A tag was deleted only when Netflix/vmaf defined a tag of the same name
pointing at the same object. All 26 are lightweight tags (object id equals
commit id). No GitHub release object was attached to any of them; the five
releases of the repository are `v1.0.0-rc.1`, `v1.0.0-rc.2` and three
`tester-*` pre-releases.

| Tag | Object | Commit | Netflix object |
|---|---|---|---|
| `v1.0.2` | `2c06ddf5fd8903d631a1275e5fb2169bcbbc7212` | `2c06ddf5fd8903d631a1275e5fb2169bcbbc7212` | `2c06ddf5fd8903d631a1275e5fb2169bcbbc7212` |
| `v1.1.6` | `d13611cd859ba267ca0e6f36cb19e55483fc04b2` | `d13611cd859ba267ca0e6f36cb19e55483fc04b2` | `d13611cd859ba267ca0e6f36cb19e55483fc04b2` |
| `v1.2.0` | `6657d1753c5774427d0a584f1265d54d9dc0b7ff` | `6657d1753c5774427d0a584f1265d54d9dc0b7ff` | `6657d1753c5774427d0a584f1265d54d9dc0b7ff` |
| `v1.3.1` | `49c2efb1f750c30fe5fd338a0a19f9973d7dc255` | `49c2efb1f750c30fe5fd338a0a19f9973d7dc255` | `49c2efb1f750c30fe5fd338a0a19f9973d7dc255` |
| `v1.3.4` | `f1225dc5366c4bd0ed2a315e4a21c88e42a4fc1e` | `f1225dc5366c4bd0ed2a315e4a21c88e42a4fc1e` | `f1225dc5366c4bd0ed2a315e4a21c88e42a4fc1e` |
| `v1.3.5` | `a6957a081a4bdb6643662f3172edbaa22394919b` | `a6957a081a4bdb6643662f3172edbaa22394919b` | `a6957a081a4bdb6643662f3172edbaa22394919b` |
| `v1.3.6rc` | `3b517cd18d4fd6544fcd52e0fa70d5ef8a0a0a02` | `3b517cd18d4fd6544fcd52e0fa70d5ef8a0a0a02` | `3b517cd18d4fd6544fcd52e0fa70d5ef8a0a0a02` |
| `v1.3.7` | `9605229b4515064eca25526a149f7a704d2c3eeb` | `9605229b4515064eca25526a149f7a704d2c3eeb` | `9605229b4515064eca25526a149f7a704d2c3eeb` |
| `v1.3.7rc` | `1b0f6efb832b0575ca1a852aa3cb29330f43ae97` | `1b0f6efb832b0575ca1a852aa3cb29330f43ae97` | `1b0f6efb832b0575ca1a852aa3cb29330f43ae97` |
| `v1.3.9` | `1b1d75db6bf44b62e9755121559c951c03199b2c` | `1b1d75db6bf44b62e9755121559c951c03199b2c` | `1b1d75db6bf44b62e9755121559c951c03199b2c` |
| `v1.3.12` | `a1cb186ff5930bf02406fa01f52e2f7e30b823df` | `a1cb186ff5930bf02406fa01f52e2f7e30b823df` | `a1cb186ff5930bf02406fa01f52e2f7e30b823df` |
| `v1.3.13` | `fbb9d3ecda8cc2bd80ecbdd63f877825216045be` | `fbb9d3ecda8cc2bd80ecbdd63f877825216045be` | `fbb9d3ecda8cc2bd80ecbdd63f877825216045be` |
| `v1.3.14` | `fcf8c9e695b16f9f3b43af9a4a0c3fb792fc071a` | `fcf8c9e695b16f9f3b43af9a4a0c3fb792fc071a` | `fcf8c9e695b16f9f3b43af9a4a0c3fb792fc071a` |
| `v1.3.15` | `e434247f8bf8f5ec36ba87062656583c073394fd` | `e434247f8bf8f5ec36ba87062656583c073394fd` | `e434247f8bf8f5ec36ba87062656583c073394fd` |
| `v1.5.1` | `35c6044d8483a3ab9528f1cc81f75738efe4f4c0` | `35c6044d8483a3ab9528f1cc81f75738efe4f4c0` | `35c6044d8483a3ab9528f1cc81f75738efe4f4c0` |
| `v1.5.2` | `19e02b9315d8f30366a70b2cc24f022ad6f64bae` | `19e02b9315d8f30366a70b2cc24f022ad6f64bae` | `19e02b9315d8f30366a70b2cc24f022ad6f64bae` |
| `v1.5.3` | `47950a6a74b3aa8c483caf3f1cdbfeda7d176086` | `47950a6a74b3aa8c483caf3f1cdbfeda7d176086` | `47950a6a74b3aa8c483caf3f1cdbfeda7d176086` |
| `v2.0.0` | `9db0c56ce6d4019456a84828f9ed8d8d9da0ff09` | `9db0c56ce6d4019456a84828f9ed8d8d9da0ff09` | `9db0c56ce6d4019456a84828f9ed8d8d9da0ff09` |
| `v2.1.0` | `2e1b24d8344c8e8dbb770699ac5beae8b11463d4` | `2e1b24d8344c8e8dbb770699ac5beae8b11463d4` | `2e1b24d8344c8e8dbb770699ac5beae8b11463d4` |
| `v2.1.1` | `8ba4a4b84beb40f97fa01d902e45dde69b18b517` | `8ba4a4b84beb40f97fa01d902e45dde69b18b517` | `8ba4a4b84beb40f97fa01d902e45dde69b18b517` |
| `v2.2.0` | `0511e05320d67238cf514104566b9d3f969963b7` | `0511e05320d67238cf514104566b9d3f969963b7` | `0511e05320d67238cf514104566b9d3f969963b7` |
| `v2.2.1` | `9451ff498402e8f0a912161e5f8dea4de6b54ae2` | `9451ff498402e8f0a912161e5f8dea4de6b54ae2` | `9451ff498402e8f0a912161e5f8dea4de6b54ae2` |
| `v2.3.0` | `5c1c9bfaad599796921b59873e9c3ed74a29c6ff` | `5c1c9bfaad599796921b59873e9c3ed74a29c6ff` | `5c1c9bfaad599796921b59873e9c3ed74a29c6ff` |
| `v2.3.1` | `f2661673a078718ddfc1ddb6042048c1b1284946` | `f2661673a078718ddfc1ddb6042048c1b1284946` | `f2661673a078718ddfc1ddb6042048c1b1284946` |
| `v3.0.0` | `17a67b238ce0539bdeafdc95961abac64fa16ea8` | `17a67b238ce0539bdeafdc95961abac64fa16ea8` | `17a67b238ce0539bdeafdc95961abac64fa16ea8` |
| `v3.0.0-rc` | `9ae6f905ead677e11156550df99e17e222d014f5` | `9ae6f905ead677e11156550df99e17e222d014f5` | `9ae6f905ead677e11156550df99e17e222d014f5` |

## What stayed

Fork tags: `v1.0.0-rc.1`, `v1.0.0-rc.2`, `tester-20261004-4d3792b3`,
`tester-20261004-860050c3`, `tester-windows-20261004-2889f963`,
`archive/eupl-relicense-v1` and `tiny-blobs-v1`.

Netflix tags that were never on `origin` (a local clone with the `upstream`
remote fetched with tags holds them; do not push them):

| Tag | Object |
|---|---|
| `v3.1.0` | `6375a4be62fd2673bba2c356b26867d8af01a685` |
| `v3.2.0` | `3f9e02af258a5c0e30124fc585a3c3af90126dee` |
| `v3.2.1` | `f85a853692a8c730d0270cd733c8bb30b5b93b7c` |

## Recreate a tag

```bash
git tag <name> <object> && git push origin refs/tags/<name>
```

The push guard (below) refuses this push by design. Restoring a Netflix tag
is a deliberate act that brings back the Go `@latest` defect above; do it only
from a clone without the hook and say why in the PR.

## Keeping them out

- `scripts/git-hooks/check-push-tags.py` runs in the lefthook `pre-push` stage.
  It refuses a pushed tag that equals a Netflix tag in the JSON list, and a
  tag whose name matches none of the fork's patterns (`vX.Y.Z[-rc.N]`,
  `tester-[windows-]<date>-<sha8>`, `archive/<topic>`, `tiny-blobs-vN`).
- `git config remote.upstream.tagOpt --no-tags` stops `git fetch upstream`
  from importing Netflix tags into a clone; see
  [Upstream parallel](release.md#upstream-parallel).
- `python3 scripts/release/delete-inherited-upstream-tags.py` (dry run; with
  `--apply` it deletes) re-checks `origin` against Netflix's current tags and
  refuses a tag whose object differs.
