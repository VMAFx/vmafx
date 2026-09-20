# Research-2070: Intel NEO release fetch fail-closed audit

## Scope

The audit covered `dev/scripts/fetch-intel-neo.py`, the build-time resolver for
the matched Intel compute-runtime, gmmlib, Level Zero, and IGC packages selected
by `NEO_VER`.

## Findings

- A bearer token attached to an API request could follow a cross-host redirect.
- Downloads wrote directly to their final path, so interruption could leave a
  partial package that looked complete to a later step.
- Retry handling did not cover the complete download operation.
- Release metadata accepted ambiguous asset matches, control characters, and
  unbounded response bodies.
- Checksum, Debian archive, and conflict-marker failures could leave corrupt
  output behind.
- `dev/Containerfile` accepted `GITHUB_TOKEN` through `ARG`. Docker BuildKit's
  native check rejected that transport with `SecretsUsedInArgOrEnv`; build
  arguments can be retained in image metadata and provenance.

## Credential-transport verification

Docker Engine 29.8.1, Buildx 0.37.1, and Compose 5.5.1 were exercised locally.
An unset or empty Compose `environment` secret is presented to BuildKit as an
empty secret, while a raw build that omits `--secret` presents no secret. The
NEO fetch instruction treats both cases as anonymous and uses the token only
when the secret value is non-empty.

The selected transport follows Docker's build-secret contract: the client
supplies `id=github_token`, and only the NEO metadata-fetch `RUN` consumes it.
The token is not an `ARG`, `ENV`, layer, or provenance value.

## Resolution

The resolver now validates every URL as GitHub-hosted HTTPS, attaches
authorization only to exact `api.github.com` requests, strips it on cross-host
redirects, bounds metadata reads, and writes downloads through a same-directory
temporary file before atomic replacement. Asset resolution is exact and
fail-closed, names are decoded and checked for control characters, and all
validation failures clean up their output. Optional authentication now uses a
BuildKit secret for raw Docker, Compose, and CI callers; anonymous builds remain
the default when no token is supplied.

## Evidence

```text
python3 -m pytest -q dev/scripts/test_fetch_intel_neo.py
14 passed

python3 -m unittest discover -s scripts/ci/tests -p test_dev_container_build_secret.py
6 tests passed

env -u GITHUB_TOKEN docker compose --project-directory "$PWD" \
  -f dev/docker-compose.yml config --quiet
passed

env -u GITHUB_TOKEN docker build --check --file dev/Containerfile \
  --target libvmaf-build .
passed with zero warnings

docker buildx build --progress=plain --file dev/Containerfile \
  --target gpu-sdks --secret id=github_token,env=GITHUB_TOKEN .
passed; all five NEO 26.35 packages downloaded, checksum-verified, package-validated, and installed
```

The hermetic tests cover redirect credential handling, transient retry,
truncated output cleanup, ambiguous assets, malformed checksums, binary package
validation, and conflict-marker rejection without contacting GitHub.

The build-secret contract tests mutation-check the forbidden ARG/ENV form, a
required secret, missing Compose and raw-build wiring, and missing anonymous
build documentation. [ADR-1271](../adr/1271-neo-buildkit-github-token-secret.md)
records the transport decision.

An uncached anonymous build reached the changed NEO fetch layer and failed
closed when GitHub returned its anonymous shared-IP rate limit; it did not leave
or install a partial package. Repeating the same target with the optional
BuildKit secret completed the full NEO 26.35 fetch and install. This proves both
credential paths without pretending that an externally rate-limited anonymous
request succeeded.

This is not a claim that the whole image build is warning-free. Its compiler
and third-party build output exposed 509 pre-existing warning lines: 407 in the
multi-backend libvmaf build, 31 while building Intel VPL GPU runtime, 69 while
building FFmpeg, and two from cloning annotated tags. The CPU fixture also
reported the existing singular-covariance warning on 6 of 192 solves. Those
warnings are separate cleanup work; recording them here prevents a successful
credential-transport check from hiding them. They are active inputs to PR #1487's
warning-clean CUDA/dev-container sweep and PR #902's FFmpeg refresh; this PR
does not grant an upstream, vendor, or legacy-code exemption.
