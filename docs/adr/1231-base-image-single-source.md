<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1231: Container bases and toolchain versions come from one config file

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: build, ci, docs, security

## Context

Every container base image in the tree was pinned by hand at the point of use,
and the copies drifted. `docker/Dockerfile.controller` and
`docker/Dockerfile.operator` were still building on Debian 12 and shipping on
`distroless-*-debian12` months after the rest of the tree moved to Debian 13.
The `golang` digest quoted in those two files' own header comments did not
match the digest in their own `FROM` line. Two CUDA pins sat on Ubuntu 24.04
while the sibling stage next to them used 26.04.

Worse, four of the pins were not `FROM` lines at all. `dev/Containerfile`
lifted the Go toolchain out of a `golang:1.27-bookworm` image with
`COPY --from=<image>`, and `docker/Dockerfile.node` pulled CUDA, ROCm and
oneAPI runtime libraries the same way. Those are base-image pins in every
sense that matters — they decide the libc and the SDK the shipped artifact
carries — but they are invisible to anyone scanning for `FROM`, which is why
they were the most stale things in the repository.

The same versions also appear in CI workflows, and drifted there independently.
ROCm was `7.2.4` in `build.yml` and `7.2.3` in `libvmaf-build-matrix.yml`, so CI
validated a ROCm the published images never shipped. The Level Zero loader
existed at **four** versions at once — `v1.18.5`, `v1.28.0` twice, `v1.29.0`,
and `1.32.0` — and a skew there does not fail a build: it surfaces at runtime as
*"No device of requested type available"*.

Per user direction: define one release configuration and one dev configuration
in a single place, rather than changing a version by hand in a dozen files —
covering the whole toolchain, not just container bases.

## Decision

`build-config.env` at the repository root is the single source of truth for
every container base image and toolchain generation. No Dockerfile names a
base image directly: each takes it as a build argument whose default mirrors
the config, so a plain `docker build` still works with no wrapper and CI can
override any base with `--build-arg`. Vendor runtime libraries are pulled from
*named stages* rather than `COPY --from=<image>`, so every pin is an ordinary
`FROM` that both the gate and Renovate can see.

Workflows cannot `source` a file at parse time, so their `run:` steps source it
at run time (`set -a; . ./build-config.env; set +a`), and
`scripts/ci/load-build-config.sh` exports every knob into `$GITHUB_ENV` for
steps that need it earlier.

`scripts/ci/check-base-image-single-source.sh` enforces all of it —
including `scripts/ci/check-workflow-versions.py` for the workflow literals —
and `make base-images-sync` rewrites the mirrors from the config.

Renovate is reconfigured to match. Its `docker` manager understands
`ARG X=image` + `FROM $X`, so left alone it would bump the Dockerfile mirrors
and leave `build-config.env` behind — failing this ADR's own gate on Renovate's
PRs. One custom manager now matches both the config line and the `ARG` form
across all nine wired files, so a bump lands every copy in a single PR, and a
`packageRule` disables the built-in manager on exactly those files.
`docker/dev/*.Dockerfile` deliberately stays with the built-in manager.

This is deliberately the same shape as
`scripts/ci/check-default-model-single-source.sh`: one authoritative value,
mirrors permitted where a consumer cannot read the authority directly, and
drift fatal.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Config + ARG mirrors + drift gate** (chosen) | One edit point; `docker build` still works standalone; gate makes drift impossible; pins stay `FROM` lines Renovate already understands | The value appears twice (config + ARG default) | The duplication is *enforced*, which is the same trade the default-model gate already makes and the repo already trusts |
| `ARG` with no default, values only from a wrapper | Truly one copy of each value | Every bare `docker build` fails; forkers and IDE tooling break; CI must thread args through six workflows | Breaks the "fork, edit, build" path that motivated the change |
| `docker buildx bake` with an HCL config | Idiomatic; one file drives all targets | Six workflows call `docker/build-push-action` directly; converting them is a separate, larger change with its own risk | Does not fit the existing CI shape; revisitable later |
| Renovate-only, no config file | Zero new machinery | Renovate updates digests but cannot unify twelve *independent* pins onto one generation, and never sees `COPY --from` | Does not solve drift, which was the actual problem |

## Consequences

- **Positive**: changing the Debian generation, the Go release, or a GPU SDK is
  a one-line edit. Four Debian 12 pins and two Ubuntu 24.04 pins are gone. Four
  base pins that were hidden inside `COPY --from` are now visible to the gate,
  to Renovate, and to a human reading the file.
- **Positive**: `docker/Dockerfile.controller` now runs as UID 65532. Its old
  pin was `cc-debian12` *without* the `:nonroot` suffix, so it ran as root; the
  shared runtime pin is `:nonroot`. The `USER` line is written explicitly in
  the file rather than left implicit in the tag. Both listen ports (8080, 9090)
  are above 1024, so nothing needs a capability.
- **Negative**: a base image value exists in two places (config and ARG
  default). The gate is what makes that safe, so the gate is now load-bearing.
- **Neutral / follow-ups**: ROCm and oneAPI keep their Ubuntu 24.04 pins under
  an explicit, self-closing exemption in the gate. ROCm 7.2.4 → 10.0.0 is
  PR #1386, which carries the matching HIP changes. oneAPI 2025 → 2026.1 is the
  immediate follow-up to this ADR and is a restructure, not a pin swap — see
  [the research digest](../research/1231-base-image-single-source.md) for the
  soname, glibc and GPU-driver measurements that determine its shape.

## Supply-chain impact

- **New dependencies**: none.
- **Removed dependencies**: `gcr.io/distroless/cc-debian12`,
  `gcr.io/distroless/static-debian12`, `golang:1.27-bookworm`,
  `nvidia/cuda:13.3.1-*-ubuntu24.04`. Removal is complete for Debian 12: no
  live reference remains outside historical records.
- **Build-time fetches**: unchanged. Every base remains digest-pinned; the gate
  now *fails* on any pin that is not.
- **CVE surface delta**: narrows. Debian 12 bases move to Debian 13, and the
  controller stops running as root.
