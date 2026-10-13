- CI: the required-checks aggregator (`.github/workflows/required-aggregator.yml`)
  no longer passes a required context that never reported, was skipped, was neutral
  or was cancelled. Every owed context must report success; the exemptions are the
  contexts the CI tier does not owe, the pull-request-only contexts on a push or a
  dispatch, and a hardware lane whose switch is off, each named in the log
  ([ADR-3293](docs/adr/3293-aggregator-owed-contexts-must-succeed.md)).
