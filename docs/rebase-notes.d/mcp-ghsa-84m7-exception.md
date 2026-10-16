## Dependency-advisory exception rule (2026-10-15)

`.config/lint-exceptions.d/dependency-advisory.toml` is fork-only: it names lock files of open
Dependabot alerts that no reachable version fixes. No upstream file is touched; an upstream sync
has nothing to merge here. Delete the `mcp` entry once Renovate moves `semgrep` to a release that
allows `mcp` 1.30.0.
