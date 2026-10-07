<!-- markdownlint-disable MD051 -->
# Release process

A release is cut by merging a release-please PR, finalizing the changelog
and publishing the resulting draft release. This page is the maintainer
runbook for that flow and for the signing and verification steps that
follow.

Pushes to `master` drive a
[release-please](https://github.com/googleapis/release-please-action)
workflow that maintains one release PR. Merging that PR creates a draft
release; publishing the draft creates the tag and triggers the full build,
signing and publication pipeline. The [automation flow](#automation-flow)
below shows each step.

## Version scheme

Releases follow ordinary SemVer tags, `vX.Y.Z`:

- `X` changes for incompatible public-surface changes.
- `Y` changes for backward-compatible features.
- `Z` changes for backward-compatible fixes.

The fork's first release is `v1.0.0`. It is preceded by release
candidates `v1.0.0-rc.N`; `v1.0.0-rc.1` and `v1.0.0-rc.2` are published.
Netflix's tags (`v1.0.2` to `v3.2.1`) are not tags of this repository: the 26
that had been copied into it were deleted on 2026-10-05 ([the record](inherited-tags.md),
[ADR-1805](../adr/1805-delete-inherited-netflix-tags.md)). A `vX.Y.Z` tag you
see in a clone that is not a fork release came from `git fetch upstream` with
tags. The 3.2.x baseline that used
to be in the manifest was a source-version alignment with Netflix's
SONAME, not a fork release.

release-please runs in prerelease mode (`versioning: prerelease`,
`prerelease-type: rc`, `release-please-config.json`), and
`.release-please-manifest.json` holds the current candidate
(`1.0.0-rc.2`). The final cut flips `prerelease` to `false` once the
candidate sequence passes (see
[Cutting a release from fragments](#cutting-a-release-from-fragments)).
[ADR-1151](../adr/1151-vmafx-first-release-1-0-0.md) governs the first
release and supersedes
[ADR-1127](../adr/1127-single-semver-release-stream.md)'s "start at
v3.2.1"; [ADR-1201](../adr/1201-release-candidates-before-1-0-0.md) adds
the candidate channel.

Example progression:

```text
v1.0.0  # first VMAFx release
v1.0.1  # patch release
v1.1.0  # backward-compatible feature release
v2.0.0  # incompatible public-surface release
```

## First-release candidate responsibilities

[ADR-1341](../adr/1341-rc-correctness-benchmark-retrain-sequence.md)
separates the first release into ordered evidence stages. The stage-to-tag
map has been revised twice:

- [ADR-1421](../adr/1421-rc3-rc8-candidate-map.md) maps the stages to tags
  and supersedes the candidate mapping of
  [ADR-1352](../adr/1352-rc-phase-shift-plus-one.md), which had added the
  stabilisation candidate after rc.1.
- [ADR-1490](../adr/1490-rc3-rc9-candidate-map-cpu-capability.md) inserts
  RC7 and moves the later candidates up by one, so each phase number
  matches its tag.

- [ADR-2001](../adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)
  folds the cloud-native work, a GStreamer element, an OBS-ready API and real-
  time FFmpeg GPU scoring into RC4, RC5 and RC8, the full CPU SIMD ladder into
  RC7 and legacy GPU build variants into RC6, all without new numbers, and
  replaces the post-1.0 milestones with the themed releases 1.1 to 1.5
  (below). It is the last change of the map until 2.0, apart from bugs and
  findings.
- [ADR-2342](../adr/2342-rc-map-amendment-2026-10.md) records the scope
  decisions of 2026-10-06 and 2026-10-07 without new numbers: more RC4 work
  packages, more RC5 tool-consolidation work and the post-1.0 placement of the
  issues filed since.

The user-facing version of this map is [the roadmap](../roadmap.md).

### Candidate map

| Candidate | Proves | Must not be used to claim |
| --- | --- | --- |
| `v1.0.0-rc.1` (RC1) | Release-blocking correctness is closed or explicitly deferred, required checks pass on the exact head, and outside testers can run the bounded hardware-validation/report path | Final performance or production-trained model quality |
| `v1.0.0-rc.2` (RC2) | The dependency and fix train since rc.1 meets the RC1 bar: no confirmed release blocker or untriaged `docs/state.md` row, required checks pass on the exact head, and the tester report path still works | Final performance or production-trained model quality |
| `v1.0.0-rc.3` (RC3) | Twin exactness: every GPU and SIMD twin returns the CPU extractor's scores bit for bit or carries a measured tolerance recorded in an ADR, 8K cells included; no SYCL kernel uses scratch memory; no extractor or twin overflows at 8K or 16K with 16-bit samples (ADR-1880). Rows that only a device the project does not own can close (unreached CUDA and HIP architectures, Xe-LP, the Windows GPU builds) are carried past rc.3 with a stated reason ([ADR-1707](../adr/1707-rc3-exit-without-outside-hardware.md)) | Performance, production-trained model quality, or exactness on a device the release notes list as not yet verified |
| `v1.0.0-rc.4` (RC4) | The whole `vmaf_v1.0.16_3d0h` path (cambi, speed_chroma, integer adm3, integer motion3, model prediction) runs in Rust, bit-identical to C, with the C ABI unchanged; a device-resident frame is scored through the import API with fences and no host copy of pixel data ([ADR-1829](../adr/1829-rc4-zero-copy-import.md)); the new VMAFx API and its generated surfaces, the VMAFx-named FFmpeg filters and provenance on every score pass with the golden-data gate run through the compatibility library ([ADR-1868](../adr/1868-candidate-map-2026-10-05.md)); the versioned scoring API contract and server mode with observability are generated from the same definition; the native `vmafx` GStreamer element and the conformance run of upstream's gst-plugins-bad `vmaf` element on the compatibility `libvmaf.so.3` pass; the API is ready for OBS Studio (texture import including OpenGL interop, asynchronous window scores, bounded queues); real-time FFmpeg GPU scoring with `n_stats` works ([ADR-2001](../adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)); the language bindings are generated from the definition; the FFmpeg patch series is redesigned from `0001` after an audit of everything VMAFx uses in FFmpeg, and builds without warnings on every measured compiler; host input of every semi-planar and packed layout, RGB with a stated matrix and every bit depth from 8 to 16 is bit-exact with the planar path; `motion2` and `motion3` are incremental so live windows complete before the flush; Vulkan frames import on CUDA, SYCL and HIP; `docs/principles.md` is per language with each missing gate proven by a planted defect; the observability package and the cloud-native platform (state out of the processes, scaling on queue depth, CRDs, proto, OpenAPI and the Helm schema generated from the definition) pass their end-to-end tests ([ADR-2342](../adr/2342-rc-map-amendment-2026-10.md)) | Performance, or production-trained model quality |
| `v1.0.0-rc.5` (RC5) | One implementation per behaviour across GPU twins, host code and tools, with `libgpudispatch` extracted (it folds in the per-backend import code RC4 wrote); the new metrics (ΔE-ITP, PU21, NIQE, BRISQUE, Y-FUNQUE+, HDR-SSIM, HDR-MS-SSIM, XPSNR) and the Metal SpEED twins are written once on it, each twin exact or within a measured libm bound; device-targeted scoring (device profiles mapped onto `nvd` / `rdh`, one decode scored for many targets, ADR-1880); containers, Helm chart and a kind plus kuttl test setup, the operator, the controller / node split and the GPU pool arbiter in `libgpudispatch` ([ADR-2001](../adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)); the Go tools replace the Python MCP server and `vmaf-tune`; live alignment of two feeds with timecode, interlaced video, region masks, container input through FFmpeg libraries, bits per pixel and BD-rate, HandBrake support, the minimal run-result timeline and one reusable build-and-provenance workflow for every image pass their tests, and the Vulkan compute experiment has a written verdict ([ADR-2342](../adr/2342-rc-map-amendment-2026-10.md)) | Performance, or production-trained model quality |
| `v1.0.0-rc.6` (RC6) | The checked-in GPU capability table matches the vendor toolchains (CI drift check), dispatch and kernel parameters read it, and every kernel, the RC5 twins included, passes the static audit for every target; the table lists the legacy build variants (CUDA 12.x for sm_50 to sm_72, the Intel legacy compute runtime for Gen9 to Gen11, every AMD target the pinned ROCm compiler emits), each bit-exact, and declares each backend's and device's format envelope (up to 16K with measured memory limits, 8 to 16 bits, chroma layouts, odd and portrait sizes), every row test-backed (ADR-1880) | Measured performance on any device |
| `v1.0.0-rc.7` (RC7) | The checked-in CPU capability table matches the compile flags and runtime gates (CI drift check), no SIMD kernel contains an instruction outside the feature set its gate guarantees, and every dispatch level is bit-exact against scalar under emulation (details below the table); every extractor has a bit-exact kernel at every useful ISA level of x86-64, AArch64, RISC-V RVV 1.0, POWER VSX and LoongArch LSX/LASX, with qemu-user CI and the golden gate where no hardware exists (ADR-2001); the CPU format envelope is declared and test-backed in the same table (ADR-1880); every extractor with an original implementation is proven against it in a reference conformance column, the default is reference-exact and Netflix's behaviour is a named compatibility mode that the golden gate runs in ([ADR-2343](../adr/2343-reference-exact-default-compat-mode.md)) | Measured performance on any processor |
| `v1.0.0-rc.8` (RC8) | Benchmarks, profiles, and tuning results are comparable, reproducible, and still numerically correct on the tested hardware, throughput per resolution up to 16K included and distributed throughput across nodes ([ADR-2001](../adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)); the training tooling is ready (data hygiene, evaluation tooling, HDR conversion checks) and the mini retrain passes every stage | Completion of the real retraining programme |
| `v1.0.0-rc.9` (RC9) | The one-shot real retrain, started only when every precondition of #1246 holds, trained on reference-exact features (the shipped v1 models read compatibility-mode features until then), and its quality, provenance, registry, signing, and golden-data gates pass on the tuned tree | That no later repair candidate can be needed |

### After 1.0.0

Every minor release runs its own candidate cycle in the order of the rules
below: features first, then deduplication, tests and bug fixing, then the
capability tables with benchmarks and tuning, then training where models
change, then the release ([ADR-2001](../adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)).

| Release | Theme |
| --- | --- |
| 1.1 | Integrations and live quality: the OBS Studio plugin (#2239), #2148, #2147, #2144, #2146, #2159, no-reference mode in the plugins (#2413), the live P.1204 monitor (#2417), AI-generated video scoring (#2418), WebRTC, streaming outputs, libVLC and cookbook recipes |
| 1.2 | Encoder feedback, embedding and platforms: the rest of #2067, #2164, #2156, encoder-side predictors (#2416), the VLC plugin (#2358), mobile and WebAssembly targets |
| 1.3 | New metrics with exact twins: #2165, #2167, #2166, picks from #2168, our own no-reference models (#2415), foveated scoring (#2419) |
| 1.4 | Metric A/B comparison, the best current mix and more training data: #2240, #2241 |
| 1.5 | The next model generation: #2242 |
| 2.0 | Breaking changes only: the `libvmaf.h` compatibility library removed (ADR-1852 D7), the C++23 core, the rest of #1254 |

### RC7 evidence in detail

- The instruction audit is per function, for x86 and aarch64.
- Emulation covers Intel SDE CPU models and qemu for aarch64 NEON and
  SVE2 at more than one vector length.
- Reports from real Xeon or Apple Silicon machines are extra evidence, not
  a requirement.

### Rules for candidates

Candidate tags are immutable. RC1 to RC9 name the planned candidate for each
responsibility; if a stage finds a correctness defect, land the fix and rerun
the affected evidence before advancing. A later repair candidate may be cut
without moving benchmark work into RC1 to RC7 or real training before RC8
evidence is accepted. Speed that RC3 gives up for exactness is recorded as a
tuning row and recovered in RC8, never traded back for a tolerance. The final
`v1.0.0` follows accepted RC9 evidence.

Every report and acceptance record identifies:

- the exact commit and the published artifact or image digest;
- fixtures, host and device, drivers and runtimes, tool versions;
- commands, exit codes and raw logs.

A green check or benchmark from a different head is not evidence for the
candidate being evaluated.

Ordinary Renovate and version-update PRs are not frozen between candidates.
They merge under the same required checks, review, digest/pin policy and
component-specific validation as any other change. Coordinated major SDK,
toolchain and base-image updates keep their specialised validation. If one
merges after evidence was collected, rerun the checks or measurements it
can affect against the new exact head.

The VMAFx release stream advances independently of Netflix/vmaf. Upstream
alignment remains recorded in sync commits and release notes, not encoded in
the tag.

### Product version vs. libvmaf ABI SONAME

These are two different numbers and only the first one moves at release time.

| Number | Owner | Value today | Moves when |
| --- | --- | --- | --- |
| **Product version** | release-please | `1.0.0-rc.2` (the manifest value); `1.0.0` at final | Every release. Covers the `vX.Y.Z` tag, `core/meson.build`'s `project(version:)`, `compat/python-vmaf`, the three fork-local Python distributions (`ai/`, `dev-llm/`, `mcp-server/vmaf-mcp/`), and the Helm chart's `appVersion`. It does **not** reach `libvmaf.pc` — see ADR-1235. |
| **ABI SONAME / interface version** | hand-maintained | `vmaf_soname_version = '3.0.0'` at `core/meson.build:38`, shipping `libvmaf.so.3` **and advertised as `libvmaf.pc`'s `Version:`** | Only on a C API change. **The 1.0.0 cut does not reset it.** |

`libvmaf.so` keeps its 3.x SONAME while the product goes to 1.0.0, and
`libvmaf.pc` advertises that same 3.x interface version rather than the
product version. The coupling is deliberate and load-bearing:

- unpatched upstream FFmpeg's `configure` requires `libvmaf >= 2.0.0`;
- the fork's own `ffmpeg-patches/` require `libvmaf >= 3.0.0` for the SYCL
  and DNN entry points.

A `.pc` advertising `1.0.0` satisfies neither. That is how the first
release candidate failed the three FFmpeg lanes and `Docker Image Build`:

```text
Package 'libvmaf' has version '1.0.0-rc.1', required version is '>= 2.0.0'
```

See [ADR-1235](../adr/1235-pkgconfig-advertises-abi-version.md).

!!! warning
    Do not "align" the two numbers. The comments at
    `core/meson.build:38` and at the `pkg_mod.generate()` call in
    `core/src/meson.build` say so at the source.

### Coordinated version markers

`release-please-config.json`'s `extra-files` array is the single list of
coordinated version markers, and `scripts/release/verify-release-version.sh`
derives its check list from that same array — add a release surface there and
the preflight covers it automatically. Each listed file must carry **exactly
one** `x-release-please-version` marker.

Deliberately *not* coordinated, and deliberately unannotated:
`deploy/helm/vmafx/Chart.yaml`'s `version:` (chart packaging, nothing publishes
it), the three `Cargo.toml` crate versions, the root `pyproject.toml` tooling
aggregate, and the `ARG VMAFX_VERSION=dev` defaults in `docker/Dockerfile.*`
(the publish workflows always pass the real value as a build argument). Each
carries an inline comment saying so.

## Automation flow

The flow in five steps (the diagram at the top of this page shows it
graphically):

1. **release-please watches master.** On each push it inspects Conventional
   Commit headers (`feat:`, `fix:`, `docs:`, `chore:`, `ci:`, …) to determine
   whether a release is warranted. If so, it opens or updates one release PR
   that bumps the root manifest and every coordinated version marker.
2. **Finalize the generated release PR.** After all other release changes are
   merged, run `make docs-render` (the rendered changelog, ADR index and
   rebase notes are written when pull requests land, so this is normally
   a no-op) and the fragment rollover with the PR's exact version and UTC
   date. Commit that result as the release PR's final
   change. Any later fragment invalidates the cut and must be rolled again.
3. **Merging the release PR** creates a draft GitHub release. It does not yet
   create the public release tag.
4. **An authenticated operator publishes the draft.** Publication creates the
   `vX.Y.Z` tag and emits the `release.published` event. This explicit gate is
   deliberate: publication is the irreversible step, and a human is the only
   actor allowed to take it.
5. **The publication workflows** check out the published release's immutable
   tag rather than the default branch. The Docker workflows derive their image
   tags from the same `github.event.release.tag_name`; the other release jobs
   build libvmaf binaries and Python wheels. Every workflow signs and attests
   its outputs, runs its own smoke checks, and publishes only after those gates
   pass.

```figure
merge-release-flow
```

### What actually gates a release PR

A release PR's diff is `.release-please-manifest.json` plus the coordinated
version markers. Under the Required Checks Aggregator's absent-means-pass rule
([ADR-0313](../adr/0313-ci-required-checks-aggregator.md)) that would let every
path-routed build and test gate select nothing and still resolve green — correct
for a doc-only PR, wrong for the one PR whose merge creates a release. Two
things now prevent that:

- `.release-please-manifest.json` and `release-please-config.json` are members
  of the `c_core` selector in `.github/ci-impact.json`, so a release PR really
  does select the C build and the Netflix golden gate.
- The aggregator keeps a `mustReport` list — `Release Script Contract`,
  `Netflix CPU Golden`, `Ubuntu gcc+DNN` — that fails instead of passing when a
  check is *absent* on a `release-please--` head ref.

### Release-bot identity

`release-please.yml` authenticates as a GitHub App (or, as a fallback, a
personal access token), never as `GITHUB_TOKEN`.

#### Why

PRs and pushes made with `secrets.GITHUB_TOKEN` do not trigger further
workflow runs. That is a GitHub loop-breaker, not a configuration mistake.
It is why release PRs used to land as `action_required` with zero jobs: the
sole required context could never report and the PR sat `BLOCKED` behind an
admin bypass.

Every step in the job (both release-please invocations and both read-only
`gh api` probes) uses the token resolved by the `Resolve the release-bot
token` step. The job's own `GITHUB_TOKEN` keeps the workflow default
`contents: read` and holds no write scope at all.

#### Identity modes

Two identities are accepted, in this order; the third row is the
no-credentials state:

| Mode | Credential | When it is used |
| --- | --- | --- |
| `app` | `RELEASE_BOT_APP_ID` + `RELEASE_BOT_PRIVATE_KEY` | Preferred. Minted per run by `actions/create-github-app-token`, short-lived, scoped to this repository. |
| `pat` | `RELEASE_BOT_TOKEN` | Fallback when the App secrets are absent. Needs `repo` + `workflow`. |
| `none` | — | Pipeline stays idle: warning on `push`, error on `workflow_dispatch` (ADR-1171). |

!!! note
    Prefer the App. A PAT is broader-scoped and longer-lived than an App
    installation token, and it carries whatever scopes its owner granted
    rather than only the two this workflow needs (Contents, Pull requests).
    The PAT path exists because registering a GitHub App has no API path:
    it is a browser flow and its private key is downloadable only once, at
    creation, so requiring the App made the whole release pipeline block on
    a manual step. Swap to the App when it exists and delete the PAT secret.

The same identity, resolved the same way (App, else PAT, else a failing step that
names the missing secrets), creates the tag and release of the macOS tester bundle
in `macos-tester-bundle.yml`: the job token cannot create the tag of a commit that
differs from master's tree in `.github/workflows/`. Only that one step uses it
([tester maintainer notes](tester-image.md)).

Whichever mode is active, the resolved token is masked with `::add-mask::`
before it reaches any later step.

#### One-time App setup

!!! warning
    One-time maintainer setup. Until the credentials exist the workflow
    never falls back to `GITHUB_TOKEN`, because that would recreate an
    unmergeable release PR.

Without credentials:

- On every push to `master` the first step emits a warning annotation and
  skips every write step, so the run ends green and idle.
- On a manual `workflow_dispatch` the missing credentials are an error and
  the run fails, because an operator asked for a release step (ADR-1171).

`scripts/release/check-release-bot-secrets.sh` checks the two secret names
locally and is part of the `/prep-release` dry run, so a release cannot be
attempted without the identity. To set it up:

1. Create a GitHub App owned by the `VMAFx` org (name it e.g.
   `vmafx-release-bot`). Repository permissions: **Contents: read & write**
   and **Pull requests: read & write**. Nothing else.
2. Install it on `VMAFx/vmafx` only.
3. Generate a private key and add two repository secrets:
   - `RELEASE_BOT_APP_ID` — the App's numeric ID.
   - `RELEASE_BOT_PRIVATE_KEY` — the full PEM, including the
     `-----BEGIN…`/`-----END…` lines.

The installation token is minted per run and revoked when the job ends;
there is no long-lived credential and nothing to rotate on a schedule. A
personal access token would also work mechanically but ties the release
stream to one human's account and expires; see ADR-1151's alternatives.

### Process gates on the release PR

Six process gates in `rule-enforcement.yml` are required contexts (see the
[branch-protection inventory](#master-branch-protection) below). Four of
them encode authoring discipline and cannot be satisfied by a PR nobody
writes by hand:

| Gate | Why a release PR cannot pass it unaided |
| --- | --- |
| Deliverables Checklist | release-please's body is the rendered changelog: no six-item checklist, no opt-out sentinel. |
| Doc-Substance Gate | the coordinated version markers include `mcp-server/vmaf-mcp/pyproject.toml`, which the gate path-maps to a mandatory `docs/mcp/` edit. |
| docs/state.md Gate | the changelog body can carry a `closes #N` line inherited from a commit subject, which trips the bug-shaped heuristic. |
| FFmpeg-Patches Surface Sync | diff-driven, and a version-marker bump is not a patch-stack change. |

Each of those four jobs therefore runs `scripts/ci/release-pr-exempt.sh` first
and skips its work step when the predicate says the PR is machine-generated.
The predicate requires **both** a `release-please--` head ref **and** a bot
author, so pushing a branch named `release-please--anything` does not disarm a
required gate. The jobs still report — they report green, not absent, which
keeps them distinguishable from a path-filter skip.

The remaining two stay armed on release PRs on purpose:

- `Release Script Contract` proves the cut ran and that the one-shot
  `release-as` / `bootstrap-sha` fields are gone. It also runs
  `scripts/ci/tests/test-release-pr-exempt.sh`, so the predicate that
  disarms the other four is itself proven on every PR, release PR
  included.
- `ADR Collision Guard` is diff-driven and trivially green when no ADR is
  added.

To dry-run the predicate locally:

```bash
HEAD_REF=release-please--branches--master--components--vmafx \
  PR_AUTHOR='vmafx-release-bot[bot]' PR_AUTHOR_TYPE=Bot \
  bash scripts/ci/release-pr-exempt.sh
```

### Publication environments

Before publication, repository setup must provide two protected environments:

- `release-publish` for GHCR writes, release-blob signing, build-provenance
  attestations, and GitHub Release attachment;
- `pypi-publish` for the `vmaf-mcp` Trusted Publisher identity.

Each must accept selected tag refs matching `v*` and require the release
reviewer.

!!! warning
    GitHub auto-creates a referenced environment that does not exist, with
    an empty rule set. A write-bearing job naming a missing environment
    therefore runs straight through with no approval gate, and
    `scripts/release/tests/test-publication-environment-binding.sh` only
    greps the YAML, so it cannot see that server-side drift.

`supply-chain.yml`'s `validate-release` queries both environments over the
API and fails closed unless each carries a `required_reviewers` protection
rule. That preflight is read-only and runs before any job holds write or
OIDC scope.

The two build-provenance jobs (`provenance` for the native files,
`mcp-provenance` for the `vmaf-mcp` distributions) hold OIDC and attestation
write scope, so they run in `release-publish` as well, with `contents: read`.
Each writes its attestation bundle as a workflow artifact; the protected
attachment job is the sole job that uploads the two bundles to the GitHub
Release ([ADR-1356](../adr/1356-release-provenance-attest.md)).

### Native Linux release layout

Download the CLI and the whole `libvmaf.so*` chain into one directory; the
CLI finds its library next to itself. The native files attached by
`supply-chain.yml` are currently Linux ELF artefacts.

#### Download

Meson builds a three-name dynamic-library chain: `libvmaf.so`, its
ABI SONAME such as `libvmaf.so.3`, and its ABI real name such as
`libvmaf.so.3.0.0`. GitHub artifact and release downloads do not preserve
symlinks, so the workflow publishes all three names as identical regular-file
assets. Each name is hashed, inventoried in both native SBOMs, signed, and
listed as a subject of the native build-provenance attestation.

Restore the raw CLI asset's executable bit after the download. The CLI's
only RUNPATH entry is `$ORIGIN`,
the directory the CLI itself sits in, so it loads `libvmaf.so.3` from there
without `LD_LIBRARY_PATH`, from any working directory, as long as the files
stay together:

```bash
mkdir vmafx-linux && cd vmafx-linux
gh release download v1.0.0 --repo VMAFx/vmafx \
  --pattern vmaf --pattern 'libvmaf.so*'
chmod +x vmaf
./vmaf --version
readelf -d vmaf | grep RUNPATH   # Library runpath: [$ORIGIN]
```

!!! note
    The `v1.0.0-rc.1` CLI predates this: it carries Meson's build-tree
    RUNPATH `$ORIGIN/../src`, which finds nothing next to the downloaded
    file. Run that release candidate as
    `LD_LIBRARY_PATH="$PWD" ./vmaf --version`.

#### Runtime requirements

The bundle needs x86-64 Linux with glibc 2.38 or newer and the libstdc++ of
GCC 12 or newer. It is compiled on the fork's Debian 13
release track (the `release-build` stage of the dev container, glibc 2.41;
[ADR-1354](../adr/1354-native-bundle-release-track.md)), the same base the
published container images are built on. Its newest symbol versions are
`GLIBC_2.38` (the C23 `strtol` / `sscanf` entry points) and
`GLIBCXX_3.4.30`. Every release checks that it runs on:

| System | glibc | Checked by |
| --- | --- | --- |
| Ubuntu 24.04 (GitHub-hosted `ubuntu-24.04`) | 2.39 | `verify-native-artifacts` runs the full clean-environment verifier on the runner |
| Distroless `cc-debian13` (`RELEASE_RUNTIME_CC`, the base of the CLI image) | 2.41 | `verify-native-artifacts` starts the downloaded CLI in that image with no `LD_LIBRARY_PATH` |
| Debian 13 (the `release-build` stage) | 2.41 | the release build runs the verifier inside the stage it compiled in |

Newer distributions, such as Ubuntu 26.04 (glibc 2.43), load it as well.
It does not load on Ubuntu 22.04 (glibc 2.35) or Debian 12 (glibc 2.36);
both fail with `version 'GLIBC_2.38' not found`. On those systems use the
production containers or build from source. Check a host with
`ldd --version`.

The bundle is CPU-only and built without the ONNX Runtime tiny-AI backend,
so it needs no GPU runtime or `libonnxruntime`.

#### How the layout is built

`scripts/release/build-native-release-artifacts.sh` sets the CLI's RUNPATH
`$ORIGIN` on the staged copy with `patchelf`; the build tree keeps Meson's
`$ORIGIN/../src`. The clean-environment release gate,
`scripts/release/verify-native-release-artifacts.sh`, exercises the layout
before signing and again after an artifact upload/download round trip, with
no `LD_LIBRARY_PATH`. It requires the CLI's RUNPATH to be exactly `$ORIGIN`
(no other entry and no `DT_RPATH`), its `DT_NEEDED` entry to resolve to the
staged SONAME file, and `vmaf --version` to report the release version.

Windows `vmaf.exe` remains a CI build artifact, not a GitHub Release asset,
and this workflow currently publishes no macOS native CLI or dylib. Use the
production containers or build from source for those platforms until
platform-specific release bundles are introduced.

### Canonical build environment (ADR-1346)

[ADR-1102](../adr/1102-phase4b9-container-only-publishing.md) requires native
release artifacts to be built inside `dev/Containerfile`.
[ADR-1346](../adr/1346-hosted-slim-container-release-build.md) (superseding
ADR-1178's self-hosted runner) meets that on a GitHub-hosted runner, and
[ADR-1354](../adr/1354-native-bundle-release-track.md) puts the compile on the
Debian 13 release track that `build-config.env` assigns to published
artifacts. In short:

| Concern | What happens |
| --- | --- |
| Where | `build-artifacts` in `.github/workflows/supply-chain.yml`, on `ubuntu-latest`. |
| How long | An uncached stage build took under a minute and the release compile under a minute on four workstation CPUs. The job has a 60-minute limit. No workstation or self-hosted runner has to be online. |
| Compile | `scripts/release/build-native-release-artifacts.sh` inside the image. |
| Rehearsal | The Dev Container PR gate builds the same stage with the same script on every container-affecting PR. |
| Verification | `verify-native-artifacts` on `ubuntu-24.04`. |
| Recovery | A timed-out or failed build is re-run with the recovery dispatch below, which rebuilds the stage from the same tag. |

#### Where

`build-artifacts` builds the `release-build` stage of the release tag's own
`dev/Containerfile` with
`scripts/ci/build-dev-container-stage.sh release-build`. That stage is the
digest-pinned Debian 13 base `RELEASE_BUILDER_BASE` plus Debian archive
packages (GCC 14, Meson, Ninja, NASM, `xxd`, `patchelf`).

- It downloads nothing from third parties and needs no GitHub token.
- The build uses no external layer cache and no registry.
- Archive packages resolve when the stage is built, so a later rebuild of
  the same tag may use newer packages from a Debian point release.
- The exception is patchelf, which edits the published CLI:
  `dev/Containerfile` pins it to Debian 13's package version
  (`PATCHELF_VERSION`).

#### Compile

The script runs with `docker run --pull never --network none` as the
runner's user. It:

1. refuses to build unless the checkout is `GITHUB_SHA`;
2. configures Meson with `--buildtype=release -Denable_avx512=true
   -Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled
   -Denable_tests=false`;
3. stages the bundle, setting the staged CLI's RUNPATH to `$ORIGIN`;
4. stamps `container-build-provenance.txt` with
   `scripts/ci/check-container-build.sh --stamp`;
5. runs `scripts/release/verify-native-release-artifacts.sh`.

The unit tests are left out because Debian 13's GCC 14.2 crashed at random
while link-time optimising them; the release does not ship them, and other
CI jobs build and run them.

#### Rehearsal

The Dev Container PR gate runs the same invocation against a local tag
named after `.release-please-manifest.json`'s version, so a change that
would break the release compile fails there.

#### Verification

`verify-native-artifacts` runs on `ubuntu-24.04`, the oldest GitHub-hosted
image that can load the bundle. It names that label rather than
`ubuntu-latest` so the check cannot drift to a newer glibc. It:

- checks the stamp with `--verify`;
- exercises the downloaded CLI in a clean environment;
- starts the CLI in the release runtime image (`RELEASE_RUNTIME_CC`,
  distroless `cc-debian13`) with no `LD_LIBRARY_PATH`, so the RUNPATH
  `$ORIGIN` is proven there too.

The stamp is signed with Cosign and attached as a release asset.

### Release recovery dispatches

Use the manual supply-chain and Docker dispatches only to recover an existing,
published GitHub release (a final release or a candidate). To re-run a workflow
unchanged, run it at the immutable tag ref and pass that same tag as its input:

```bash
tag=vX.Y.Z
gh workflow run supply-chain.yml --ref "$tag" -f tag="$tag"
gh workflow run docker-publish-production.yml --ref "$tag" -f tag="$tag"
gh workflow run docker-publish-operator-node.yml --ref "$tag" -f tag="$tag"
```

Where each workflow may run:

- `supply-chain.yml` only runs at the tag: its release binaries and their
  signatures must come from the tag's own workflow.
- The two image workflows can also be dispatched on `master` when the tag's
  build recipe itself was broken. That run builds the tag's source with
  `master`'s `docker/` recipe and signs as `master`
  ([ADR-1347](../adr/1347-image-recovery-from-default-branch.md); see
  [Recovering a release's container images](#recovering-a-releases-container-images)).

Each preflight verifies the coordinated versions and the published GitHub
release before any write or OIDC job starts. The protected deployment
environments still apply on recovery runs; approval authorizes the
write-bearing jobs only, after the read-only preflight has proved the
tag/ref/release identity.

The moving `latest` container tag is resolved from the repository's newest
published release rather than from the trigger event, so a recovery dispatch
repoints `latest` when it is recovering the newest release and leaves it alone
otherwise. (Before ADR-1151 the guard was `github.event_name == 'release'`, so
a recovery run left `latest` pointing at the broken original digest.)

## ADR index regeneration policy

`docs/adr/README.md` is the rendered index of every ADR in the fork. Its
"Index" table is generated from per-ADR fragments under
`docs/adr/_index_fragments/<slug>.md`. The renderer is
[`scripts/docs/concat-adr-index.sh`](../../scripts/docs/concat-adr-index.sh)
(see [ADR-0221](../adr/0221-changelog-adr-fragment-pattern.md) for why the
pattern exists and [ADR-2197](../adr/2197-render-generated-docs-at-landing.md)
for who writes the render).

### Rendered at landing, not in the pull request

A pull request does **not** carry `CHANGELOG.md`, `docs/adr/README.md`,
`docs/adr/by-tag/`, `docs/adr/titles.md`, `docs/research/titles.md` or the
fragment block of `docs/rebase-notes.md`
([ADR-2197](../adr/2197-render-generated-docs-at-landing.md)).
`scripts/ci/deliverables-check.sh` refuses a pull request that edits one of
them. The outputs are written by one command, `make docs-render`:

- **per landing batch:** the merge train runs it at the batch tip and commits
  the result as `chore(docs): render generated changelog and ADR index`, in the
  same push as the batch, so every master tip is rendered;
- **at the release cut:** see
  [Cutting a release from fragments](#cutting-a-release-from-fragments);
- **by hand:** to see what the render will write, run it in a scratch
  worktree; it is safe to run at any time and writes nothing else.

`make docs-render-check` fails when a render is stale. The master push runs it
(the `Docs` job), so a push that skipped the render turns master red instead of
passing silently. A pull request does not run it: `make docs-fragments-check`
checks the fragments themselves (changelog sections, ADR index rows, rebase-note
headings) and the outputs that stay in the pull request (exact-twin table,
AGENTS indexes, charts, vendored assets).

The render needs full history: the order of the index rows and of the rebase
notes is the order the fragments landed on the branch, read by
`scripts/docs/fragment-order.py`. A shallow clone fails loudly.

### Adding a new ADR

**When adding a new ADR (the common case)** — write the fragment
`docs/adr/_index_fragments/<slug>.md` as part of the same PR, and nothing
else: do not touch `README.md` or `_order.txt`. `_order.txt` is the frozen list
of the rows that existed when the index moved to fragments; every other
fragment follows it in landing order.

### Fixing drift

**When fixing drift between fragments and `README.md` (this sweep's case)**
— run `make docs-render-check` to capture the full diff,
then audit each row against the four drift classes:

- **Silent loss** — fragment exists, README is missing the row.
  Regenerating with `--write` keeps the fragment's row.
- **Orphan content** — README has a row, fragment does not exist.
  Backfill the fragment from the README row (the row content already
  reflects the ADR's accepted state). Do **not** delete the row without
  evidence the ADR is genuinely stale (`Status: Withdrawn` or
  `Superseded` in the ADR file body, plus the underlying decision being
  moot).
- **Reformatted** — same content, different shape (column order, status
  spelling, slug case). Regenerate; the fragment is canonical.
- **Duplicate** — the same row appears more than once in `README.md`,
  usually from a stale append-only edit. Regenerate; the fragment is
  emitted exactly once.

After every fragment-side fix, run `--write` once and verify the README
diff matches the audit's expected shape (rows preserved, duplicates
collapsed, missing rows restored). Reviewers can re-run `--check` against
the rebuilt branch and expect a clean exit.

**Renumbered slugs.** When the dedup sweep referenced in the script's
header comment renumbers an ADR (e.g. `0270-saliency-…` → `0286-saliency-…`),
the fragment must be **renamed** to match the new slug — not duplicated. A
rename of a fragment listed in the frozen `_order.txt` changes that entry too;
a rename of any other fragment moves its landing position, so rename in the
commit that adds it. The fragment body's
`[ADR-NNNN](NNNN-slug.md)` link must match the renumbered slug; mismatches
silently render rows that point at non-existent ADR files. The
fragment-vs-ADR-file slug audit is one line:

```bash
for f in docs/adr/_index_fragments/[0-9]*.md; do
    base=$(basename "$f" .md)
    [[ -f "docs/adr/$base.md" ]] || echo "STALE FRAGMENT: $f"
done
```

## Signing

All release artefacts are signed via
[Sigstore keyless](https://docs.sigstore.dev/cosign/overview/) using the
repository's GitHub OIDC identity. No long-lived signing keys live in the
repo or in CI secrets.

### What is signed

- **Release blobs** (`libvmaf.so*`, `vmaf`, `models.tar.gz`,
  `container-build-provenance.txt`, `THIRD_PARTY_NOTICES.txt`,
  `licenses.tar.gz`, optional `u2netp_mirror.{onnx,pth}`):
  cosign sign-blob bundles attached to the GitHub Release, plus one GitHub
  build-provenance attestation (`actions/attest-build-provenance`) that lists
  every file as a subject. The attestation is an in-toto statement with the
  [SLSA v1 build-provenance](https://slsa.dev/spec/v1.0/provenance) predicate,
  signed through Sigstore; it is stored with the repository's attestations and
  attached as `vmafx-build-provenance.sigstore.json`. SPDX + CycloneDX SBOM
  attached; the SPDX SBOM is also attested on the same subjects with
  `actions/attest` (`gh attestation verify <file> --repo VMAFx/vmafx
  --predicate-type https://spdx.dev/Document/v2.3`). See
  [ADR-1356](../adr/1356-release-provenance-attest.md).
- **Licences of the release blobs** ([ADR-1513](../adr/1513-production-artifact-licensing.md)):
  `build-native-release-artifacts.sh` writes `THIRD_PARTY_NOTICES.txt` (every
  component, licence and copyright line of `libvmaf` and `vmaf`, computed from
  the SPDX headers of the files the build compiled) and `licenses.tar.gz` (the
  same file with every licence text), and `models.tar.gz` carries its own
  `licenses/` directory for the models. Both are written and checked by
  `tools/rc1-tester/image/licensing.py` (artifact kinds `release-native`,
  `release-models`); the build fails on a file without a recorded licence.
  Both tarballs are `gzip -9n` ([ADR-1591](../adr/1591-package-compression.md));
  the container layers are zstd at BuildKit's strongest level and need Docker
  Engine 23.0 or later to pull
  ([ADR-1594](../adr/1594-zstd-images-zopfli-zips.md), table in [Artifact
  publishing policy](publishing.md#compression)).
- **`vmaf-mcp` Python package** (wheel + sdist): cosign sign-blob bundles, a
  GitHub build-provenance attestation attached as
  `vmaf-mcp-provenance.sigstore.json`, an SPDX SBOM attestation on the wheel
  and sdist, and PEP 740 attestations stored alongside the PyPI artefact
  (Trusted Publishing, no token). See
  [ADR-0166](../adr/0166-mcp-server-release-channel.md).
- **Production container images** (`ghcr.io/vmafx/vmafx:<tag>` and the
  `-cuda13` / `-rocm10` / `-oneapi2026` (also tagged `-oneapi2025`) / `-server`
  variants): cosign keyless signature plus a GitHub-native build-provenance
  attestation
  (`actions/attest-build-provenance`). See
  [ADR-0902](../adr/0902-signing-and-attestation-audit.md).
- **Go service images** (`ghcr.io/vmafx/vmafx-server:<tag>`,
  `ghcr.io/vmafx/vmafx-controller:<tag>` (since
  [ADR-1589](../adr/1589-helm-controller-workload.md)),
  `ghcr.io/vmafx/vmafx-operator:<tag>`, and
  `ghcr.io/vmafx/vmafx-node:<tag>`): the same cosign signature, CycloneDX SBOM,
  and GitHub-native build provenance, emitted by
  `docker-publish-operator-node.yml`. Each is built per architecture on a
  native runner and published as one multi-arch index, which carries the
  signature, SBOM and provenance
  ([ADR-1349](../adr/1349-native-arch-node-image-build.md)).

### Consumer verification recipes

The OIDC identity is the workflow path inside the repo. The release blobs
and MCP wheel come from `supply-chain.yml`; the container images come from
`docker-publish-production.yml`.

```bash
# The release tag to verify. v1.0.0-rc.1 and v1.0.0-rc.2 use the
# slsa-verifier recipe further down.
tag=v1.0.0

# Release blob. Every vmaf/libvmaf.so* asset has a matching FILE.bundle.
cosign verify-blob --bundle vmaf.bundle vmaf \
  --certificate-identity \
    "https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml@refs/tags/${tag}" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# Release blob, build provenance (ADR-1356). gh fetches the attestation from
# GitHub. Works for every native asset; the vmaf-mcp wheel and sdist from the
# GitHub Release verify the same way.
gh attestation verify vmaf --repo VMAFx/vmafx \
  --signer-workflow VMAFx/vmafx/.github/workflows/supply-chain.yml \
  --source-ref "refs/tags/${tag}"

# The same check against the attached bundle, without the attestations API.
# Fetch the Sigstore trusted root once while online; after that no network
# access is needed.
gh attestation trusted-root > trusted_root.jsonl
gh attestation verify vmaf --repo VMAFx/vmafx \
  --bundle vmafx-build-provenance.sigstore.json \
  --custom-trusted-root trusted_root.jsonl \
  --signer-workflow VMAFx/vmafx/.github/workflows/supply-chain.yml \
  --source-ref "refs/tags/${tag}"

# vmaf-mcp wheel on PyPI. The PyPI integrity API supplies the PEP 740
# provenance; pypi-attestations binds it to the expected source repository.
pypi-attestations verify pypi \
  --repository https://github.com/VMAFx/vmafx \
  pypi:vmaf_mcp-1.0.0-py3-none-any.whl

# Container image, cosign route. Replace DIGEST with the actual sha256 digest.
cosign verify ghcr.io/vmafx/vmafx@sha256:DIGEST \
  --certificate-identity \
    "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@refs/tags/${tag}" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# Container image, GitHub-native attestation route (added by ADR-0902).
gh attestation verify oci://ghcr.io/vmafx/vmafx@sha256:DIGEST --repo VMAFx/vmafx

# Go server/operator/node images use their own workflow identity.
cosign verify ghcr.io/vmafx/vmafx-node@sha256:DIGEST \
  --certificate-identity \
    "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-operator-node.yml@refs/tags/${tag}" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

`supply-chain.yml` runs the provenance recipe above against the attached bundle
for every subject before the bundle can reach the release, so a release whose
provenance does not verify is never published.

v1.0.0-rc.1 and v1.0.0-rc.2 predate ADR-1356. They carry
`slsa-github-generator` provenance as `vmafx-build-provenance.intoto.jsonl` and
`vmaf-mcp-provenance.intoto.jsonl` instead of the `.sigstore.json` bundles and
have no GitHub attestation. Verify those with
[`slsa-verifier`](https://github.com/slsa-framework/slsa-verifier):

```bash
slsa-verifier verify-artifact vmaf \
  --provenance-path vmafx-build-provenance.intoto.jsonl \
  --source-uri github.com/VMAFx/vmafx --source-tag v1.0.0-rc.2
```

An image rebuilt by a [recovery run](#recovering-a-releases-container-images)
was signed by the workflow on `master`, not at the tag, so its identity ends
in `@refs/heads/master`. Such an image carries the label
`io.vmafx.build-recipe=<master commit>`; every v1.0.0-rc.1 image was built
this way. Check the label, then verify with the `master` identity:

```bash
docker buildx imagetools inspect ghcr.io/vmafx/vmafx:v1.0.0-rc.1 \
  --format '{{json .Image}}' | jq -r '.. | .Labels? // empty | ."io.vmafx.build-recipe"'
cosign verify ghcr.io/vmafx/vmafx@sha256:DIGEST \
  --certificate-identity \
    "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@refs/heads/master" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# vmafx-server, vmafx-operator and vmafx-node come from the other workflow.
cosign verify ghcr.io/vmafx/vmafx-node@sha256:DIGEST \
  --certificate-identity \
    "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-operator-node.yml@refs/heads/master" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

For a recovered image the tag identities above fail with `no matching
CertificateIdentity found`, and so does `gh attestation verify` pinned to the
tag with `--source-ref refs/tags/<tag>` or `--source-digest <tag commit>`. The
GitHub build-provenance attestation records the run that built the image:
`refs/heads/master` and the recipe commit. The built source is the tag's
commit, which a recovery run also writes into
`org.opencontainers.image.revision`.
The v1.0.0-rc.1 images were recovered before that label was corrected, so
theirs names the recipe commit `a919f3596` instead of the tag's `ce00cf245`.

#### Post-push smoke jobs

The post-push smoke jobs in both Docker workflows run the matching cosign
verification recipe before pulling an image, with the identity of the run
that signed it (`@${GITHUB_REF}`: the tag, or `master` for a recovery run).
After that:

- the production workflow executes the CPU CLI and the Python 3.14 server
  entrypoints;
- the Go-service workflow starts the Go scoring server and probes
  `/healthz` plus `/readyz`;
- it also checks the operator version and executes `vmaf --version` plus
  `ffmpeg -version` from the node image.

A signature or runtime-linkage gap fails the release rather than silently
shipping a broken image.

## CHANGELOG.md fragment workflow (ADR-0221)

The "Unreleased" block of `CHANGELOG.md` is **rendered** from per-PR fragment
files under `changelog.d/<section>/*.md` by
[`scripts/release/concat-changelog-fragments.sh`](../../scripts/release/concat-changelog-fragments.sh).
Sections follow Keep-a-Changelog order: `added` → `changed` → `deprecated` →
`removed` → `fixed` → `security`. The pre-fragment archive lives verbatim in
`changelog.d/_pre_fragment_legacy.md` and is emitted before the section
fragments so existing release-train history is preserved.

### When to add a fragment vs edit `CHANGELOG.md` directly

- **Always add a fragment, never edit `CHANGELOG.md` directly.** Drop a single
  Markdown bullet under `changelog.d/<section>/<topic>.md`. The fragment is
  the source of truth; the rendered `CHANGELOG.md` is a build artefact that a
  pull request does not carry ([ADR-2197](../adr/2197-render-generated-docs-at-landing.md)).
- **Filename convention:** lowercase kebab-case, optionally prefixed with the
  task ID (`T7-39-foo.md`) or ADR number (`adr-0312-deferral-retired.md`)
  for implicit lexical ordering within the section.
- **One fragment per PR.** Multi-surface PRs may ship multiple fragments,
  one per user-discoverable surface, each in the appropriate section.

### When to regenerate (`--write`)

`make docs-render` writes the changelog with the other rendered outputs: the
merge train runs it once per landing batch and the release cut runs it. You
need it by hand only to preview a render, or to reconcile a skew:

- `make docs-render-check` (the master push) fails on a stale render.
- Pre-existing skew (see
  [the 2026-05-08 sweep](#changelog-drift-sweep-historical-context)) is
  reconciled by rendering on master, not by a feature branch: a pull request
  that carries a render is refused.

Never edit the rendered "Unreleased" block by hand to add new entries — those
inline edits will be silently overwritten by the next regen.

### Drift classes and resolution policy

Three drift classes can develop between fragments and the rendered block:

| Class | Symptom | Resolution |
| --- | --- | --- |
| **Silent loss** | Fragment exists, no matching row in `CHANGELOG.md`. | Regenerate. The fragment is canonical. |
| **Orphan content** | Row in `CHANGELOG.md`, no matching fragment. | Backfill a fragment if the content is still relevant; delete the row otherwise. Inspect each case manually — never bulk-delete. |
| **Duplicate** | Same entry appears twice (often once from legacy archive, once from a fragment, or once inline + once from a fragment). | Regenerate. The script renders each fragment exactly once. |

`--write` is conservative: it only rewrites the `## [Unreleased]` block.
Released sections below are untouched.

### Cutting a release from fragments

Release-please has `skip-changelog: true`; it never edits `CHANGELOG.md`.
The cut is two commits on the generated release PR, made after it contains
the final manifest and version-marker updates.

1. Label the release PR `autorelease: cut` so release-please leaves the
   branch alone (see
   [Freezing the release PR while you cut it](#freezing-the-release-pr-while-you-cut-it)).
2. Render the final notes:

    ```bash
    make docs-render
    git commit -am 'docs(release): render final 1.0.0 notes'  # skip if nothing changed
    ```

3. Roll the fragments over into the versioned section. Replace the example
   version and UTC date for other releases:

    ```bash
    scripts/release/rollover-changelog-fragments.sh \
      --version 1.0.0 --date YYYY-MM-DD
    git add CHANGELOG.md changelog.d release-please-config.json docs/changelog-archive
    git commit -m 'chore(release): cut 1.0.0 changelog'
    ```

4. Push both commits, merge the release PR and publish the draft release.

The rollover requires:

- a clean tree;
- exact agreement between the root manifest and every coordinated marker;
- zero renderer drift;
- a unique target heading;
- a non-empty active source set.

It then removes the consumed fragments and legacy source, leaving their
exact content in the versioned changelog section and a SHA-256 receipt
under `changelog.d/releases/`. The removals are recoverable from Git
history. A second identical invocation is a no-op.

A long body goes to `docs/changelog-archive/X.Y.Z.md` (see `--archive-over`).
The first release's archive holds the whole fragment history and is larger than
the 1 MB `check-added-large-files` limit, so top-level Markdown files in that
directory are exempt from it
([ADR-1345](../adr/1345-changelog-archive-large-file-exemption.md)).
Nothing else in the directory is.

#### Before the cut: every tester leg green on the commit

A pull request builds only the tester legs whose inputs it changed, and a master
push skips a leg whose inputs did not change, so a leg can be red for days
unseen (the x64 SYCL Windows zip was, from 2026-10-05 to the rc.3 publish run).
Before you label the release pull request `autorelease: cut`, dispatch every leg
at the commit the release is cut on (the release pull request's base, the head
of `master`) with the **full SHA**, so the run title names its source:

```bash
SHA=$(git rev-parse origin/master)
for wf in windows-tester-bundle.yml macos-tester-bundle.yml docker-publish-tester.yml; do
  gh workflow run "$wf" -R VMAFx/vmafx --ref master -f ref="$SHA"
done
```

Then check them (the same script the cut pull request runs, see below):

```bash
GH_TOKEN=$(gh auth token) python3 scripts/release/check-candidate-legs.py --sha "$SHA"
```

It lists every leg of `scripts/release/candidate-legs.json` that is not
`success` on that commit: skipped, absent and red all count as not green, and a
dispatch titled `master` or a tag proves nothing about a commit. The `Release
Script Contract` job runs the same check on a release pull request that carries
the `autorelease: cut` label, against the pull request's base commit, so the cut
cannot merge on an unseen or red leg
([ADR-2198](../adr/2198-windows-sycl-leg-and-cut-check.md)). When `master` moves
after the dispatch, dispatch again at the new head.

#### Cutting a release candidate

A release candidate is cut the same way, with its full version, for example
`--version 1.0.0-rc.3`. The script accepts exactly the shapes the tag-time
verifier accepts, `X.Y.Z` and `X.Y.Z-rc.N`. Each candidate gets its own
`## [1.0.0-rc.3] - YYYY-MM-DD` section and
`changelog.d/releases/1.0.0-rc.3.json` receipt.

The one-shot `release-as` and `bootstrap-sha` fields are no longer in
`release-please-config.json`. The rollover still deletes them if present,
because the tag-time verifier refuses them at every tag, candidates
included.

#### Numbering later candidates

Later candidates are numbered automatically. The root package uses
release-please's `prerelease` versioning
([ADR-1348](../adr/1348-release-candidate-prerelease-versioning.md)): a fix,
feature or breaking change on `1.0.0-rc.1` gives `1.0.0-rc.2`, and so on. The
release PR stays open as a proposal; merge it only when the next candidate is
due, and check its title before cutting it. For the final release, set
`"prerelease": false`: the same strategy then proposes `1.0.0`.

!!! note
    With the earlier `versioning: default`, a fix on `1.0.0-rc.1` gave
    `1.0.1-rc.1`. The Release Script Contract job now rejects that setting
    while the manifest is a release candidate.

How a candidate moves through the rest of the pipeline:

- **Draft pause.** While a `vX.Y.Z` or `vX.Y.Z-rc.N` draft waits for
  publication, `release-please.yml` skips both release-please phases, so later
  pushes to `master` neither move nor duplicate it. More than one waiting draft
  stops the job for operator cleanup.
- **Version strings.** A release build checked out at its tag reports the
  `git describe` form from `vmaf --version`, for example
  `v1.0.0-rc.1-0-gd0f0e7e` or `v1.0.0-0-g8820048`.
  `scripts/release/verify-native-release-artifacts.sh` accepts exactly that
  form (distance 0) or the bare version, never a later commit.
- **Python distributions.** Python tools name distributions by the PEP 440
  spelling, `1.0.0rc1`, not `1.0.0-rc.1`. `supply-chain.yml` derives that
  spelling once with `scripts/release/pep440-version.sh` and matches the
  `vmaf-mcp` wheel and sdist by it.
- **Pushing the cut.** The local pre-push PR-body hook exempts the bot release
  PR, as CI's Deliverables Checklist does, through
  `scripts/ci/release-pr-exempt.sh`. The exemption needs `gh` to be
  authenticated and the PR's head ref to be the branch being pushed.

[ADR-1128](../adr/1128-fragment-owned-release-cuts.md) governs this cutover.

#### Recovering a release's container images

The image workflows (`docker-publish-production.yml`,
`docker-publish-operator-node.yml`) normally run at the release tag with the
tag's own workflow file. If an image job fails because of the build recipe,
fix it on `master`, then dispatch the workflow on `master` with the published
tag ([ADR-1347](../adr/1347-image-recovery-from-default-branch.md)).

The `release-publish` environment admits only `v*` tags, so a run on `master`
is rejected at the environment gate ("Branch "master" is not allowed to deploy
to release-publish") before any step runs. Allow `master` for the recovery
only, then take the permission away again:

```bash
# 1. Let master deploy to release-publish; the required reviewer still applies.
policy=$(gh api -X POST \
  repos/VMAFx/vmafx/environments/release-publish/deployment-branch-policies \
  -f name=master -f type=branch --jq .id)

# 2. Dispatch the recovery runs and approve their release-publish deployments.
gh workflow run docker-publish-production.yml --ref master -f tag=v1.0.0-rc.1
gh workflow run docker-publish-operator-node.yml --ref master -f tag=v1.0.0-rc.1

# 3. After both runs have finished (re-runs need the policy too), remove it.
gh api -X DELETE \
  "repos/VMAFx/vmafx/environments/release-publish/deployment-branch-policies/$policy"
gh api repos/VMAFx/vmafx/environments/release-publish/deployment-branch-policies \
  --jq '.branch_policies[] | "\(.type) \(.name)"'   # expect only: tag v*
```

The run verifies the published tag, builds the tag's source with `master`'s
build recipe, and labels every image `io.vmafx.build-recipe=<master commit>`.
The recipe is `docker/`, `Dockerfile.go-server` and `ffmpeg-patches/`, the
patch series for the FFmpeg that the node image bundles
([ADR-1350](../adr/1350-recovery-overlay-ffmpeg-patches.md)). Everything else,
including all VMAFx and libvmaf code, is the tag's.

#### Making the container images public

The five packages (`vmafx`, `vmafx-server`, `vmafx-controller`,
`vmafx-operator`, `vmafx-node`) must be public so that anyone can pull the
release images. `vmafx-controller` does not exist until the first release
that publishes it (ADR-1589): after that publish, check its visibility and, if
it was created private, switch it as described below. The organization
setting **Organization settings → Packages → Package creation → Public** decides
what a new package gets; it has been checked since v1.0.0-rc.1:

- **Setting checked.** A package first pushed from this public repository is
  created public, as `vmafx-operator` was for v1.0.0-rc.1, and no further step
  is needed.
- **Setting unchecked.** The package is created private, and the per-package
  option is greyed out ("disabled by organization administrators"). `vmafx`
  and `vmafx-server` were created this way before v1.0.0-rc.1. Once the setting
  is checked, an organization owner switches such a package once in the web
  UI: **Package settings → Danger Zone → Change visibility → Public**, then
  types the package name to confirm. The REST API cannot do this, because the
  packages endpoints offer only read and delete.

Later pushes keep the package's visibility.

Check that an anonymous client can pull:

```bash
token=$(curl -s "https://ghcr.io/token?scope=repository:vmafx/vmafx-node:pull" | jq -r .token)
curl -s -H "Authorization: Bearer $token" \
  https://ghcr.io/v2/vmafx/vmafx-node/tags/list | jq '.tags'
```

#### Freezing the release PR while you cut it

Label the release PR `autorelease: cut` before you push the cut commits.

release-please force-recreates `release-please--branches--master--…` on
every push to `master`: the branch always ends up with exactly one
bot-authored commit. The two cut commits are hand-added to that same
branch, so any merge to `master` after you push them silently destroys the
cut.

While the label is present, `release-please.yml` skips its PR-update
invocation and the branch is left alone; it still creates the draft
release once the PR merges. The procedure is:

1. Hold merges to `master` (or accept that you may have to redo the cut).
2. Label the release PR `autorelease: cut`.
3. Push the two rollover commits.
4. Merge the release PR, then publish the draft release.
5. If you abandon the cut, remove the label and release-please regenerates the
   branch from scratch on the next push.

`scripts/release/verify-release-version.sh` is the backstop: at tag time it
requires exactly one `## [X.Y.Z] - YYYY-MM-DD` heading, a matching
`changelog.d/releases/X.Y.Z.json` receipt, zero active fragments, no
`_pre_fragment_legacy.md`, and no surviving `release-as` / `bootstrap-sha`. A
tag whose cut never ran cannot publish.

### Drift-sweep cadence

CI runs `--check` on every PR (the docs-fragments lane) so new drift fails
loud. A periodic drift-sweep PR (typically once per merge train) reconciles
the pre-existing skew that accumulates when in-flight PRs add fragments
faster than `--write` is run.

#### CHANGELOG drift sweep — historical context

The 2026-05-08 sweep cleared 13 silent-loss fragments, 1 reformatted entry
(verbose inline → canonical fragment), and 2 duplicate rows
(double `### Changed` header + duplicate FastDVDnet entry). No genuine
orphans were found — every row in `CHANGELOG.md` either had a matching
fragment or lived in the legacy archive.

## Dry-running a release

Before merging a release PR, invoke the `/prep-release` skill locally. It
validates:

- All commits since the last release parse as Conventional Commits.
- The Netflix golden-data gate (CPU scalar + fixed-point) passes.
  GPU / SIMD backends are validated separately via per-backend
  snapshot tests.
- `CHANGELOG.md` renders correctly and references no removed files.
- Signing credentials (OIDC) resolve in the current CI environment.

Run the release-please preview from an origin-faithful clone, not directly
from the development checkout:

- Local tags include tags fetched from the Netflix `upstream` remote. Those
  tags do not necessarily exist in `VMAFx/vmafx` and can make a local
  preview select the wrong previous release.
- The preview clone must expose only the fork's advertised tags and the
  candidate `master` tree.
- Supply credentials through a protected file descriptor or token-file path
  so the CLI never echoes a literal token in its argument list.

See section 11 of
[AGENTS.md](https://github.com/VMAFx/vmafx/blob/master/AGENTS.md) for the
one-line summary and the `/prep-release` skill definition for the full
checklist.

## `master` branch protection

`master` is protected at the GitHub API layer: the policy in the
[agent hard rules](agent-hard-rules.md) and
[CONTRIBUTING.md](../../CONTRIBUTING.md) is enforced at the host, not just
honoured by convention. The declared rule set and its drift check are in
[repository security](repository-security.md).

- **Required status check (1):** `Required Checks Aggregator`. Branch
  protection names exactly this one context; every other gate is enforced
  through it.
- **Linear history required.** Merges are squash-or-ff-only.
- **Force-push and deletion disabled.**
- **Admin bypass kept on.** The owner can land emergency fixes that skip
  required checks; use sparingly (see the emergency-release section below).

Management: `gh api --method PUT repos/VMAFx/vmafx/branches/master/protection`
with a JSON payload. The current rule set is documented in
[ADR-0037](../adr/0037-master-branch-protection.md).

### What the aggregator requires

The aggregator's own `required` array
(`.github/workflows/required-aggregator.yml`) is the real inventory, about 80
contexts at the time of writing. Do not copy its count into branch-protection
settings. A context missing from the array can be red while the merge button
stays green ([ADR-1297](../adr/1297-ci-gate-every-reporting-check.md)).

| Group | Contexts |
| --- | --- |
| Builds | Ubuntu gcc+DNN, Ubuntu clang+DNN, Windows UCRT64 (MSYS2 UCRT64), Windows MSVC+CUDA, Windows MSVC+SYCL, Ubuntu HIP, Ubuntu gcc, Ubuntu clang, Ubuntu ARM clang, Ubuntu gcc static, macOS clang, macOS clang+DNN, macOS Metal, Ubuntu CUDA, Ubuntu CUDA static, Ubuntu SYCL, Ubuntu SYCL+CUDA, Linux Intel LLVM, macOS Clang+Metal, Windows MSVC+CUDA (full), Windows ARM64 MSVC |
| Static analysis | CodeQL, CodeQL (C/C++), CodeQL (Python), CodeQL (Actions), Pre-Commit, Python Lint, Semgrep, Semgrep OSS, Tidy Changed, Tidy Ratchet, Tidy SYCL, Cppcheck, Markdown Lint, No Conflict Markers, Go API Compatibility |
| Supply chain and docs | Dependency Review, Gitleaks, gitleaks, Scorecard PR Gate (pull requests), Scorecard Master Gate (master pushes), Licence Provenance ([guide](licence-provenance-check.md)), Docs, Docs Site Build, Doxygen Public API, ShellCheck + shfmt |
| Tests | Netflix CPU Golden, Coverage Gate, Coverage GPU, SYCL Parity (Arc A380), Sanitizers (address), Sanitizers (thread), Sanitizers (undefined), Sanitizers ASan+UBSan, Assertion Density, Twin Drift, Tiny AI, Tiny-Model Registry Validate, go vet + go test, MCP Smoke, RC1 Tester Report, vmafx-sys CI, cargo-deny |
| FFmpeg | FFmpeg Patch Stack, FFmpeg Ubuntu gcc, FFmpeg macOS clang, FFmpeg SYCL |
| Packaging and images | Docker Image Build, Dev Container Build, helm lint + template |
| Governance and HISS | Standards & Invariant Verification Gate ([ADR-1249](../adr/1249-praetor-governance-adoption.md)), HISS Replay Evidence (Linux), HISS Replay Evidence (macOS), HISS Replay Evidence (Windows), Silent-Revert Guard |
| Process gates | Deliverables Checklist, Doc-Substance Gate, docs/state.md Gate, FFmpeg-Patches Surface Sync, ADR Collision Guard, Release Script Contract |

#### Notes on the table

- The build lanes are `libvmaf-build-matrix.yml` legs and `build.yml` rows
  ([ADR-1259](../adr/1259-ci-build-matrix-as-it-runs.md)). The `build.yml`
  Windows row is named `Windows MSVC+CUDA (full)` so that it cannot stand
  in for the required `Windows MSVC+CUDA` lane.
- `Coverage GPU` and `SYCL Parity (Arc A380)` are enforced only while their
  distinct `GPU_COVERAGE_ENABLED` / `SYCL_ARC_RUNNER_ENABLED` variables are
  `true`; hosted probes prevent dispatch to missing label sets (ADR-1319).
- `Coverage Gate` (about 40 minutes) is built with
  `-fprofile-update=atomic` to survive parallel-meson SIMD-counter races
  ([ADR-0110](../adr/0110-coverage-gate-fprofile-update-atomic.md)).
- The process gates report on every non-draft PR. Four of the six
  auto-exempt the machine-generated release PR; see
  [Process gates on the release PR](#process-gates-on-the-release-pr).
  The other two stay armed there.
- A `strictMustReport` list in the aggregator holds the governance and
  always-reporting gates whose absence fails the check instead of reading
  as a path-filter skip.

!!! note
    When adding, renaming or removing a gate, update the aggregator's
    `required` array in the same PR and run
    `scripts/ci/check-aggregator-names.sh`. Branch protection's `contexts`
    list does not change, because it only ever names the aggregator.

## Emergency release (out-of-band)

If a CVE requires an out-of-band release that bypasses the release-please PR:

1. Branch off `master` into `hotfix/CVE-YYYY-NNNN`.
2. Land the fix with a `fix:` commit and a signed-off-by line.
3. Manually tag the next ordinary patch version, `vX.Y.(Z+1)`;
   release-please will reconcile on the next regular push.
4. Backport the CVE fix to any active stacked release branches.

## Upstream parallel

The upstream Netflix release process (manual version bump, manual CHANGELOG
editing, draft-a-release on GitHub) is documented at
[Netflix/vmaf — release.md](https://github.com/Netflix/vmaf/blob/master/resource/doc/release.md).
It does not apply to this fork.

Do not fetch Netflix's tags into a clone of this repository. After adding the
remote, set

```bash
git remote add upstream https://github.com/Netflix/vmaf.git
git config remote.upstream.tagOpt --no-tags
```

so `git fetch upstream` brings branches only. A tag fetched by mistake is
removed locally with `git tag -d <name>`; the lefthook `pre-push` guard
(`scripts/git-hooks/check-push-tags.py`) refuses to push a Netflix tag or a tag
name outside the fork's patterns, and release-please, tester workflows and
`archive/*` tags keep working.

## Go consumers and the inherited tags

Until 2026-10-05 the repository carried Netflix's tags, and
`go get github.com/VMAFx/vmafx@latest` resolved to Netflix's
`v3.0.0+incompatible`. Measured with Go 1.27.1 (the experiments are summarised in
[ADR-1805](../adr/1805-delete-inherited-netflix-tags.md)):

- The Go command reads retractions only from the `go.mod` of the highest
  version, and Netflix's `v1.5.3` (no `go.mod`) outranked every fork `v1.0.x`,
  so a `retract` could not take effect.
- After the deletion `GOPROXY=direct go list -m -versions github.com/VMAFx/vmafx`
  prints `v1.0.0-rc.1 v1.0.0-rc.2` and `go list -m github.com/VMAFx/vmafx@latest`
  prints `v1.0.0-rc.2`; no Netflix version is listed.
- `proxy.golang.org` keeps what it has cached: it still answered
  `@latest = v3.0.0+incompatible` and listed the Netflix versions on 2026-10-05,
  and it offers no purge. Its `@v/<version>.zip` stays served for every cached
  version by explicit request.

Until the proxy's `@latest` shows a fork version, pin an explicit version:

```bash
go get github.com/VMAFx/vmafx@v1.0.0-rc.2
go install github.com/VMAFx/vmafx/cmd/<tool>@v1.0.0-rc.2
```

or bypass the proxy cache: `GOPROXY=direct go list -m -versions github.com/VMAFx/vmafx`.
After `v1.0.0` exists, `go list -m -retracted -versions github.com/VMAFx/vmafx`
shows retracted releases too; keep the `retract` directive for the release
candidates in every later `go.mod`, because only the latest version's `go.mod`
is read.

## Release notes and the changelog archive (ADR-1233)

Two different surfaces, produced two different ways:

| Surface | Produced by | Grouped by |
| --- | --- | --- |
| GitHub Release body | GitHub, from merged pull requests | Labels, via [`.github/release.yml`](../../.github/release.yml) |
| `CHANGELOG.md` | `scripts/release/concat-changelog-fragments.sh`, from `changelog.d/` | `changelog.d/` subdirectory |

The GitHub body has no slot for hand-written text. A statement the release
must carry that no pull request title expresses, such as the rc.3 table of devices
verified and not yet verified ([ADR-1707](../adr/1707-rc3-exit-without-outside-hardware.md)),
lives in a `changelog.d/` fragment (so `CHANGELOG.md` has it) and the release
publisher adds it to the release page with `gh release edit --notes-file`, keeping
the generated body.

Because GitHub groups the release body **by label**,
[`.github/workflows/pr-type-label.yml`](../../.github/workflows/pr-type-label.yml)
derives a `type:*` label from each PR's Conventional-Commit prefix. If a release
body shows a large "Other changes" section, that labeler is not running — the
prefix is mandatory, so the label should always be derivable.

### Archiving an oversized release section

`rollover-changelog-fragments.sh --archive-over N` (default 400) keeps
`CHANGELOG.md` readable. When the rendered body exceeds N lines it is written to
`docs/changelog-archive/X.Y.Z.md` and the version section keeps a per-section
index linking to it:

```markdown
## [1.0.0] - 2026-09-07

This release collects 2345 changelog entries.
They are recorded in full, unedited, in
[`docs/changelog-archive/1.0.0.md`](docs/changelog-archive/1.0.0.md) — too long to read inline here.

| Section | Entries |
| --- | --- |
| Fixed | 1227 |
| Changed | 559 |
```

Nothing is discarded: the archive is tracked, and the rollover receipt still
records the sha256 of the complete rendered body. Pass `--archive-over 0` to
force the old inline behaviour.
