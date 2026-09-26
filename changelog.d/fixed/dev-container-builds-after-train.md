- **Dev container**: `dev/Containerfile` builds again. The `dev-mcp` stage
  now copies `requirements/`, which its hash-locked Python install reads, and
  the `Dev Container Publish` workflow passes the GitHub token as a BuildKit
  secret, so the Intel NEO release lookup no longer fails on GitHub's
  anonymous API rate limit.
