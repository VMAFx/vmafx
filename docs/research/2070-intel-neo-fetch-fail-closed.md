# Intel NEO release fetch fail-closed audit

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

## Resolution

The resolver now validates every URL as GitHub-hosted HTTPS, attaches
authorization only to exact `api.github.com` requests, strips it on cross-host
redirects, bounds metadata reads, and writes downloads through a same-directory
temporary file before atomic replacement. Asset resolution is exact and
fail-closed, names are decoded and checked for control characters, and all
validation failures clean up their output.

## Evidence

```text
python3 -m pytest -q dev/scripts/test_fetch_intel_neo.py
14 passed
```

The hermetic tests cover redirect credential handling, transient retry,
truncated output cleanup, ambiguous assets, malformed checksums, binary package
validation, and conflict-marker rejection without contacting GitHub.
