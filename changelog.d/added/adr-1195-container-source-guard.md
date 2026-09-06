- `scripts/dev/check-container-source.sh` and `dev/scripts/container-build.sh` make the
  dev container say which source revision it was built from, and refuse to build from a
  checkout that is behind `origin/master`. A rebuild run against a stale checkout produces
  an image newer than every commit while missing the commits it was rebuilt for, so
  nothing time-based detects it; the image now records `/etc/vmafx-dev-source` and an
  image that cannot say what it holds is reported as unverifiable rather than current. See
  [ADR-1195](docs/adr/1195-container-source-revision-guard.md).
