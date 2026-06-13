<!-- markdownlint-disable MD060 -->
# ADR-0384: Switch shfmt pre-commit hook from binary download to Go-source build

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `fork-local`

## Context

The pre-commit `shfmt` hook in `.pre-commit-config.yaml` used the
`scop/pre-commit-shfmt` repository with `id: shfmt`. This variant
downloads a prebuilt `shfmt` binary from `mvdan.cc` at wheel-build
time (when `pip install .` runs inside pre-commit's isolated virtualenv).

Two compounding problems caused CI to fail on every push to master:

1. **Stale cache key.** The `Cache pre-commit environments` step in
   `.github/workflows/lint-and-format.yml` used the key
   `pre-commit-${{ runner.os }}-py3.12-…` while the `actions/setup-python`
   step had been bumped to `python-version: "3.14.4"`. The key
   mismatch meant the cache never hit, so pre-commit rebuilt its
   virtualenvs from scratch on every CI run.

2. **Transient CDN 502.** Each cache miss forced a fresh download of
   the `shfmt` binary from `mvdan.cc`. On 2026-05-10 that CDN returned
   HTTP 502, causing `pip install .` (the wheel build) to fail and
   aborting the entire pre-commit job before any formatters ran.

Together the two bugs produced a hard CI failure on every push — the
pre-commit job never succeeded even on commits with no shell-script
changes.

## Decision

Two changes are applied in the same commit:

1. **Fix the cache key.** Update the cache key and restore-key prefix
   from `py3.12` to `py3.14` to match the actual Python version
   installed by `actions/setup-python`.

2. **Switch to `shfmt-src`.** Replace `id: shfmt` with `id: shfmt-src`
   in `.pre-commit-config.yaml`. The `shfmt-src` hook uses
   `language: golang` and fetches `mvdan.cc/sh/v3/cmd/shfmt` via the
   Go module proxy (`proxy.golang.org`), which is Google-hosted and
   significantly more reliable than the binary CDN. Ubuntu-latest
   runners ship a Go toolchain, so the build is fast (~5 s).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix cache key only | Minimal diff; once the cache warms, 502 won't matter | Does not eliminate the fragile CDN download; first run after any `.pre-commit-config.yaml` change still hits the CDN | Not resilient enough — one CDN hiccup recurs |
| `apt-get install shfmt` + local hook | Deterministic, no network at hook time | Breaks local `pre-commit run` on machines without apt (`shfmt` not in PATH on macOS by default) | Developer-experience regression |
| `shfmt-docker` | Immune to CDN and Go proxy outages | Requires Docker daemon on every CI runner and local machine; adds 300 MB+ to every fresh runner | Too heavyweight |
| Pin `shfmt_py` version that vendors the binary | No download at install time | No such version exists in `shfmt_py` | Option does not exist |
| Keep `id: shfmt`, add CDN retry logic | No config change | Not possible inside pre-commit's pip install step | Cannot control pip retry behaviour per-hook |

## Consequences

- **Positive**: Pre-commit job is no longer blocked by `mvdan.cc` CDN
  availability. The Go module proxy (`proxy.golang.org`) has a strong
  uptime SLA and a large read-through cache.
- **Positive**: Stale cache key bug is eliminated; the pre-commit cache
  will now hit on every push that does not modify `.pre-commit-config.yaml`.
- **Negative**: First CI run after this PR merges warms a new `py3.14`
  cache entry and builds shfmt from Go source (~5–10 s extra).
- **Neutral**: Developer machines need a Go toolchain installed for
  `pre-commit run` to build `shfmt-src`; `go install` or `apt-get
  install golang` suffices. Machines that already have system `shfmt`
  can run `shfmt` directly as a stand-alone formatter without using
  pre-commit.

## References

- `scop/pre-commit-shfmt` README: `shfmt-src` hook documentation.
- CI failure: run IDs 25631280619 (lint-and-format) + 25631280613
  (security-scans) on master push 2026-05-10.
- per user direction: fix three master CI failures without suppressing
  findings; shfmt fix chosen was option (a) / (c) hybrid per task spec.
