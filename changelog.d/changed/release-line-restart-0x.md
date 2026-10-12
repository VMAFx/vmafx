- **Release line restarts at `0.1.0`** ([ADR-3292](docs/adr/3292-release-line-restart-0x.md)):
  the next release is `v0.1.0`, followed by plain 0.x minor releases without
  candidates; 1.0.0 becomes the first stable release (Rust core phase P3 plus
  a frozen public interface), with candidates from `v1.0.0-rc.4`.
  `v1.0.0-rc.1` to `rc.3` stay published and are marked withdrawn. The Go
  module retracts them; through the public module proxy, pin `@v0.1.0`
  explicitly (see [the release guide](docs/development/release.md)).
