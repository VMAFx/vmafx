## Praetor pin 7458a220e1c9 and the managed workflows (2026-10-07)

`chore/praetor-pin-7458a220`, [ADR-2440](adr/2440-praetor-pin-7458a220.md). A rebase or sync
keeps `PRAETOR_REF` at `7458a220e1c9...` in `.github/workflows/standards-gate.yml`, the engine's
`praetor-api.yml` and `praetor-docs.yml` (never hand-edit: audit refuses a changed byte), and an
empty `push_branch_exceptions` in `.github/ci-tier.json`. A conflict in either workflow file takes
the engine's text: regenerate with `adopt --force` in a throwaway copy, copy back only those two
files. `test_praetor_managed_jobs_stop_on_a_draft_before_any_work` in
`scripts/ci/tests/test_ci_routing_contract.py` guards the draft stop.
