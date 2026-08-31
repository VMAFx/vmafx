- Repair the Helm operator Deployment and image metadata after the ADR-1119
  fx migration: runtime configuration now uses the supported
  `VMAFX_OPERATOR_*` environment variables, metrics bind to `:8080`, and
  health/readiness bind to `:8081`. The chart no longer passes removed CLI
  flags, probes the stale `:8082` port, or selects unpublished Go server,
  operator, and node image references; all current operator guides use the
  same env-only contract.
