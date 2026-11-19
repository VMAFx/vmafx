- **CI:** image signing and verification identities now come from
  `GITHUB_WORKFLOW_REF`, and `gh attestation verify` targets
  `GITHUB_REPOSITORY`, so a fork signs and verifies under its own executing
  repository instead of the hardcoded upstream slug (issue 3093).
