<!-- markdownlint-disable MD013 MD041 -->

# Container bases and toolchain versions

Shared container bases and toolchain versions are defined in
[`build-config.env`](../../build-config.env) at the repository root. The
contract covers the entries declared there; compatibility test versions need
separate review before consolidation.

To change one of these shared settings in a fork, edit that file and regenerate
its mirrors.

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

BuildKit prunes the stage when the selected target does not use it. The gate
rejects direct external `FROM` and `COPY --from` references with or without a
digest: `alpine`, `alpine:latest` and `alpine@sha256:…` all need a centrally
owned named stage. Instruction case, `--platform`, other `COPY` flags and
continued instructions do not exempt a reference. Docker documents these
forms in its [Dockerfile reference](https://docs.docker.com/reference/dockerfile/).

Declare each shared image's global `ARG NAME=value` on one physical line
before the first `FROM`, so the mirror checker and `--write` can maintain it.
`FROM ${NAME}` or `FROM $NAME` must use that declared configuration key;
an arbitrary new ARG or a fallback such as `${NAME:-alpine}` is rejected.
Use named stages or an earlier numeric stage index for `COPY --from`.

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

- **Local image consumers**: `Dockerfile.ffmpeg` extends `vmaf:latest`, built
  from the root Dockerfile. `dev/Containerfile.runner` uses
  `ARG BASE_IMAGE=vmaf-dev-mcp:local`, built from `dev/Containerfile`. These
  exceptions bind the exact file, argument (where applicable) and value.
  An unpinned tag in any other consumer is not assumed to be local.
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

## Verify the guard

```bash
python3 -m unittest discover -s scripts/ci/tests -p test_base_image_single_source.py -v
bash scripts/ci/check-base-image-single-source.sh
```

The tests run the actual gate in temporary Git repositories, without builds
or registry access. They cover external image bypasses, local exceptions,
named stages and mirror repair. The pre-commit regression hook runs when the
guard or configuration changes; CI's all-files pre-commit run includes it.

If two Dockerfiles want the same image, they share one entry. That collapsing
is the point: 25 `FROM` lines in this repo resolve to 13 pins.

## CI workflows read the same file

Version pins are not only in Dockerfiles. Before this file, ROCm was `7.2.4` in
`build.yml` and `7.2.3` in `libvmaf-build-matrix.yml`, and the Level Zero loader
existed at four versions at once. GitHub Actions cannot `source` a file at
parse time, so `run:` steps source it at run time:

```yaml
      - name: Install ROCm / HIP runtime
        run: |
          set -a; . ./build-config.env; set +a
          ...  https://repo.radeon.com/rocm/apt/${ROCM_APT_VERSION} ...
```

For a value needed by a later step or by a `with:` block, use the loader, which
writes `KEY=value` lines for every knob:

```yaml
      - name: Load build config
        run: scripts/ci/load-build-config.sh >> "$GITHUB_ENV"
```

`scripts/ci/check-workflow-versions.py` fails the build if a literal version
reappears in a workflow. The Windows SYCL leg runs under `cmd` and cannot source
the config, so it mirrors the value by hand — and that check is what keeps the
mirror honest.

## Renovate

Renovate's built-in `dockerfile` manager understands `ARG X=image` + `FROM $X`,
so if it owned the Dockerfiles it would bump the `ARG` mirrors and leave
`build-config.env` behind — failing the gate on Renovate's own PRs. Instead a
single custom manager in `renovate.json` matches **both** the config line and
the `ARG` form across every wired file, so one PR updates all 41 copies of the
affected pin together, and a `packageRule` disables the built-in manager on
those files. `docker/dev/*.Dockerfile` keeps the built-in manager, because those
pins are deliberately independent.
