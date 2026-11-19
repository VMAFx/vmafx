- **CI:** image verification identities now come from `GITHUB_WORKFLOW_REF`,
  and `gh attestation verify` targets `GITHUB_REPOSITORY`, so a fork verifies
  under the repository and workflow that execute instead of the hardcoded
  upstream slug (issue 3093).
