<!-- markdownlint-disable MD013 MD060 -->
# ADR-1285: CUDA is a coordinated pin — group it in Renovate, gate the rest

- **Status**: Accepted
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: `ci`, `build`, `cuda`, `renovate`, `fork-local`

## Context

One CUDA release is named in sixteen places across seven files, in seven
different spellings:

| Spelling | Where | Example |
| --- | --- | --- |
| `CUDA_VERSION` | `build-config.env` | `CUDA_VERSION="13.3.1"` |
| image pin | `build-config.env` + four Dockerfile ARG mirrors | `nvidia/cuda:13.3.1-devel-ubuntu26.04@sha256:…` |
| action input | `build.yml`, `libvmaf-build-matrix.yml` | `cuda: '13.3.1'` |
| installer version | the two Windows legs | `$cudaVersion = '13.3.1'` |
| installer series | the same two legs | `$cudaMajorMinor = '13.3'` |
| apt package | `build-config.env` + `dev/Containerfile` | `cuda-toolkit-13-3` |
| OCI label | `docker/Dockerfile.production-gpu` | `"VMAFX production CUDA 13.3.1 runtime"` |

Renovate could see two of them. The base-image custom manager (ADR-1231)
matches `NAME="image:tag@digest"`, which picks up `CUDA_BUILDER` and
`CUDA_RUNTIME` and nothing else. So Renovate opened #1487, a pull request that
moved the two image tags alone —
and `scripts/ci/check-base-image-single-source.sh:138` rejects an image tag
that disagrees with `CUDA_VERSION`. The pull request was structurally
unmergeable: no amount of re-running CI could make it green, because the change
it proposed was incomplete by construction.

The other half of the problem is quieter. Rule 3 of that gate knows only the
image spelling. The action inputs, the Windows installer versions and series,
the apt package names and the OCI label had no drift check at all, so any of
them could sit on an older release indefinitely with CI green.
`CUDA_APT_PACKAGE` in particular had **no consumer and no check** — it was a
knob nothing read, which `build-config.env`'s own comment forbids ("Add central
knobs only with real consumers or executable drift checks").

The residual sweep written for this ADR found a site nobody had inventoried:
the OCI `org.opencontainers.image.description` label on the CUDA runtime image,
which ships `CUDA 13.3.1` as prose in a published artifact. It was not in the
bug report, not in the gate, and not in Renovate.

## Decision

Treat the CUDA release as one dependency with sixteen call sites, owned in two
layers.

1. **Renovate owns every site it can rewrite, as one group.** A new custom
   manager resolves `CUDA_VERSION`, the two `cuda:` action inputs and the two
   `$cudaVersion` installer literals as the package `nvidia/cuda`, and a
   package rule groups every non-digest `nvidia/cuda` update under
   `CUDA release (coordinated pin)` with `automerge: false`. The image pins the
   base-image manager already owns resolve under the same package name, so all
   ten Renovate-writable sites land in one branch. Digest-only refreshes stay in
   the existing `Docker digests` batch: a new digest on the same tag needs no
   coordination.
2. **A gate owns the rest and proves the whole set.**
   `scripts/ci/check-cuda-pin-lockstep.py` discovers all sixteen sites by shape,
   compares each against `CUDA_VERSION`, and fails on any *other* CUDA release
   literal in scope. `--write` rewrites the five derived spellings, which are a
   pure function of `CUDA_VERSION`; `make cuda-pin-sync` is the entry point.

The division is not arbitrary. Renovate substitutes the whole looked-up version
into the slot its regex matched, so it can only own a slot that wants the whole
version. `$cudaMajorMinor` wants `13.4`, `cuda-toolkit-NN-N` wants `13-4`, and
the label wants prose — Renovate would write `13.4.0` into all three. Renovate's
`autoReplaceStringTemplate` can render a different string, but
`confirmIfDepUpdated` then requires the rendered template to equal the
re-extracted `currentValue` or the write is reverted, so the escape hatch does
not reach these shapes without contortions that cannot be tested offline. See
the research digest.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Group what Renovate can write, gate the derived remainder (chosen) | One branch per release; every site checked; the derived edits are one command; the gate finds sites nobody listed | Renovate's pull request still needs `make cuda-pin-sync` before it is green | The derived spellings cannot be expressed as dependency references without templates that cannot be verified offline |
| `autoReplaceStringTemplate` for the series, apt and label spellings | Renovate would produce a fully green pull request | `confirmIfDepUpdated` compares the rendered template against the re-extracted `currentValue` and reverts the write when they differ; behaviour only observable by running Renovate against a real token and registry | Shipping a config whose runtime behaviour was inferred from reading `dist/` is a worse failure mode than a pull request that needs one command |
| Derive `CUDA_APT_PACKAGE` inside `build-config.env` (`cuda-toolkit-${CUDA_VERSION%.*}`) | One fewer literal | The file is parsed by regex in `check-workflow-versions.py` and by Renovate's base-image manager, both of which read literal `KEY="value"` lines only | Breaks two existing readers to remove one literal the gate already derives |
| Make `dev/Containerfile` source `build-config.env` for the apt name | Removes a site outright | The `COPY build-config.env` lands ~80 lines after the CUDA `apt-get install`; moving it invalidates the layer cache for the whole SDK stage, and the container build cannot be exercised here | Out of scope for a Renovate-configuration fix, and unverifiable under the load constraint |
| Add the missing sites to `check-base-image-single-source.sh` rule 3 | No new file | Rule 3's key list is shape-detected from image references (`*/*` or `*:*`); a bare `13.3` or `cuda-toolkit-13-3` is neither, and the rule has no residual sweep | A separate gate keeps the shape-detection rule honest and adds the sweep that found the OCI label |
| Leave it and bump by hand | No work | #1487 is the second time this failed; the next Renovate run re-opens it | That is the defect |

## Consequences

- **Positive**: a CUDA release move is one Renovate branch plus
  `make cuda-pin-sync`. Every site is checked on every commit, including five
  that had no check before. A site added in a new file or a new spelling fails
  the build instead of drifting silently. `CUDA_APT_PACKAGE` now has the
  executable drift check `build-config.env` requires of every knob.
- **Negative**: the grouped pull request is red until the derived spellings are
  written, so it is not auto-mergeable — deliberately, since a CUDA bump wants
  review anyway. `nvidia/cuda` version bumps no longer ride the auto-merged
  `Docker digests` batch (digest-only refreshes still do).
- **Neutral / follow-ups**: `Jimver/cuda-toolkit`'s own version is *not* in the
  group — it is an action bump, not a CUDA release — but it caps which CUDA
  versions the Linux legs can install (v0.2.36 tops out at 13.3.1), so a CUDA
  bump the action cannot serve needs the action raised first. The ROCm and
  oneAPI OCI description labels in `docker/Dockerfile.production-gpu` carry the
  same class of stale-prose risk and are not covered here.

## References

- Bug: `#1487` (Renovate `nvidia/cuda` tags-only pull request, structurally
  unmergeable); `T-CI-CUDA-PIN-RENOVATE-PARTIAL-2026-09-21` in
  [`docs/state.md`](../state.md).
- Research digest:
  [`docs/research/renovate-cuda-coordinated-pin.md`](../research/renovate-cuda-coordinated-pin.md).
- Single-source precedent: [ADR-1231](1231-base-image-single-source.md).
- Gate: `scripts/ci/check-cuda-pin-lockstep.py`;
  tests `scripts/ci/tests/test_cuda_pin_single_source.py`.
- Container pin invariant:
  [ADR-0451](0451-local-dev-mcp-container.md) (`dev/Containerfile` pins
  `cuda-toolkit-13-3`).
- Renovate 44.103.6, read from the installed package:
  `dist/modules/datasource/common.js` (`applyExtractVersion`) and
  `dist/workers/repository/update/branch/auto-replace.js`
  (`confirmIfDepUpdated`).
- `req` (paraphrased): the user directed that Renovate treat every CUDA
  version site as a single group, so a release is proposed as a whole or not at
  all, and asked for a test that fails when a new CUDA-version site is added
  outside that group.
