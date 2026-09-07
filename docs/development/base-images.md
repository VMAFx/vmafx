<!-- markdownlint-disable MD013 MD041 -->

# Container base images

Every container base image in this repository is defined in one file:
[`build-config.env`](../../build-config.env) at the repository root.

If you have forked VMAFx and want it to build on a different Debian, a
different Go, or a different GPU SDK, that file is the only thing you edit.

## Quick start

```bash
$EDITOR build-config.env      # change a pin
make base-images-sync         # push it into every Dockerfile
git diff                      # review, then commit both
```

`make base-images-sync` rewrites the `ARG` defaults in every Dockerfile from
the config and then re-runs the check, so a clean run means the tree agrees
with the config.

## How it works

No Dockerfile names a base image directly. Each one takes it as a build
argument whose default mirrors `build-config.env`:

```dockerfile
ARG RELEASE_RUNTIME_CC="gcr.io/distroless/cc-debian13:nonroot@sha256:c31ff9ab…"

FROM ${RELEASE_RUNTIME_CC} AS runtime-base
```

Two consequences worth knowing:

- **A plain `docker build` still works.** The default is a real value, so you
  do not need a wrapper script or a bake file to build any image in this repo.
- **CI can override any base** with `--build-arg RELEASE_RUNTIME_CC=…` without
  editing a Dockerfile — useful for testing a candidate base before pinning it.

The mirrored defaults are what would rot, so
`scripts/ci/check-base-image-single-source.sh` fails the build if any of them
drifts from the config. It runs in `make lint-sh` and as a pre-commit hook.

### Vendor libraries come from named stages

Pulling a library straight out of a vendor image looks harmless:

```dockerfile
COPY --from=nvidia/cuda:13.3.1-runtime-ubuntu24.04@sha256:… /usr/local/cuda/lib64/libcudart.so* /usr/local/lib/
```

but that is a base-image pin — it decides which CUDA runtime the shipped image
carries — and it is invisible to anyone grepping for `FROM`. Four such pins in
this repo were the most out-of-date things in it. Declare a named stage
instead:

```dockerfile
FROM ${CUDA_RUNTIME} AS cuda-runtime-libs

COPY --from=cuda-runtime-libs /usr/local/cuda/lib64/libcudart.so* /usr/local/lib/
```

BuildKit prunes the stage when the selected target does not use it, so this
costs nothing. The gate rejects a digest-pinned `COPY --from`.

## The two tracks

| Prefix | What it is | Moves when |
| --- | --- | --- |
| `RELEASE_*` | What published artifacts are built from and ship on | Only on purpose — it changes what users run |
| `DEV_*` | The development and CI container | Freely; it may lead `RELEASE_*` to shake out a new base early |

Keep both on the same libc generation unless you have a written reason not to.
A dev container on a different libc than the release image tests the wrong
thing.

## Version knobs

The top of the config carries the human-meaningful version of each pin
(`RELEASE_DEBIAN`, `GO_VERSION`, `CUDA_VERSION`, …). The gate asserts that each
pinned tag actually carries the version its knob claims, so `RELEASE_DEBIAN=13`
cannot sit above a `debian:12` pin. That check is what would have caught the
drift this file exists to prevent.

## Everything is digest-pinned

A tag alone is not a pin: it moves under you, and reproducing a release build
six months later is the whole point. The gate rejects any entry without
`@sha256:`. Renovate updates the digests in place — see the `docker` manager in
`renovate.json`.

## Deliberate exceptions

- **`docker/dev/*.Dockerfile`** pin Alpine, Arch and Fedora on purpose. They
  exist to prove the build survives distros the release track does not use, so
  unifying their bases would defeat them. The gate skips that directory.
- **ROCm and oneAPI** are temporarily exempt from the "no Ubuntu 24.04" rule.
  Each is a major SDK migration that needs matching source changes, not a pin
  swap; both exemptions name their follow-up and delete themselves when it
  lands. See
  [ADR-1231](../adr/1231-base-image-single-source.md) and the
  [research digest](../research/1231-base-image-single-source.md).

## Adding a new image

1. Add a semantic entry to `build-config.env` — name it for its *role*
   (`RELEASE_RUNTIME_CC`), not for the file that uses it.
2. Reference it as `ARG` + `FROM ${…}` in the Dockerfile.
3. Run `make base-images-sync`.

If two Dockerfiles want the same image, they share one entry. That collapsing
is the point: 25 `FROM` lines in this repo resolve to 13 pins.
