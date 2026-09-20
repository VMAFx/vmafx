# Intel NEO release-fetch and credential-transport audit

## Scope

The audit covered `dev/scripts/fetch-intel-neo.py`, the build-time resolver for
the matched Intel compute-runtime, gmmlib, Level Zero, and IGC packages selected
by `NEO_VER`, plus the transport used to give it an optional GitHub token.

PR #1468 owns this hardening and the Intel NEO version update. PR #1487
temporarily mirrors the fetcher hardening and credential transport because it
touches both the fetcher and Containerfile, which must remain lint- and
warning-clean. After #1468 merges, rebase #1487 and prove the duplicate diff
disappears before promoting it from draft.

## Findings

- `dev/Containerfile` accepted `GITHUB_TOKEN` through `ARG`. Docker BuildKit's
  native check rejected that transport with `SecretsUsedInArgOrEnv`; build
  arguments can be retained in image metadata and provenance.
- A bearer token attached directly to a request could follow a cross-host
  redirect.
- Downloads wrote directly to their final path, so interruption could leave a
  partial package that looked complete to a later step.
- Retry handling did not cover the complete download operation.
- Release metadata accepted ambiguous asset matches, control characters, and
  unbounded response bodies.
- Checksum, Debian archive, and conflict-marker failures could leave corrupt
  output behind.
- The raw Docker workflow and Compose build both needed the same optional
  secret ID. A required secret would regress anonymous public builds.
- Documentation and the rate-limit remediation text still taught the obsolete
  build-argument form, so changing only the Dockerfile would leave callers
  broken.

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
validation failures clean up their output.

Optional authentication uses one BuildKit secret for raw Docker, Compose, and
CI callers. The secret is mounted as `GITHUB_TOKEN` only for the NEO fetch
`RUN`, with `required=false`, and the fetcher receives `--github-token` only
when the mounted value is non-empty. Anonymous builds therefore remain the
default when no token is supplied.

A repository checker and mutation tests bind the Dockerfile, Compose, workflow,
operator documentation, and fetcher remediation text. Native Docker and Compose
`--check` commands are warning-fatal before the expensive image build.

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

env -u GITHUB_TOKEN docker compose --project-directory "$PWD" \
  -f dev/docker-compose.yml build --check dev-mcp
passed with zero warnings
```

The hermetic fetcher tests cover redirect credential handling, transient retry,
truncated-output cleanup, ambiguous assets, malformed checksums, binary package
validation, and conflict-marker rejection without contacting GitHub.

The build-secret contract tests mutation-check the forbidden ARG/ENV form, a
required secret, missing Compose and raw-build wiring, and missing anonymous
build documentation. [ADR-1271](../adr/1271-neo-buildkit-github-token-secret.md)
records the transport decision.
