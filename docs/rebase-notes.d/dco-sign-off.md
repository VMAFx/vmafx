## DCO sign-off check (2026-10-08)

`community-dco`, [ADR-2462](adr/2462-dco-sign-off-required.md). The job
`dco-sign-off` in `.github/workflows/rule-enforcement.yml`, its entry
`'DCO Sign-off'` in the `required` list of `required-aggregator.yml`,
`scripts/ci/check-dco.py` with its bot list `BOT_LOGINS`, and `:gitSignOff` in
`renovate.json` are fork-authored. A sync that rewrites either workflow keeps
the job and the aggregator entry together (`check-aggregator-names.sh` fails
when they diverge). `scripts/ci/tests/test_check_dco.py` reads the wiring.
