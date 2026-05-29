**CI workflow permissions hardening**: added explicit `permissions: contents: read` to
`go-ci.yml` and `rust-ci.yml`, completing the repo-wide least-privilege policy. All 24
workflows now carry an explicit top-level permissions block, preventing over-broad
GITHUB_TOKEN grants regardless of org default settings. (ADR-0791)
