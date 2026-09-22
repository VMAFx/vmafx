# Developer control scripts

`merge_train_guard.py` (ADR-1244) gates every merge-train mutation. Preserve:

- master-only selection
- release/hold/owner exclusions on every action
- exact-head leases
- rebase-before-ready ordering
- error propagation

Never reuse or force-remove existing owner's checkout. Full local gate
receipt = both Make commands run on clean source; zero required checks and
handwritten pass flags do not count as validation. Gateway changes require
its disposable-repository tests plus operator-guide update at
`docs/development/merge-train.md`. Runtime scripts stay local state, never
shipped; reviewing and migrating running processes differs from shipping
source.

`check_repository_security.py` checks repository security policy (ADR-1248,
amended by ADR-1252) against named active master ruleset and its effective
rules. Preserve:

- fixed repository/HTTPS-host targeting
- exact lists and scalar types
- GitHub Actions origin on aggregate check
- bypass count verified against **declared** actor list, even when REST
  hides actors

Policy declares exactly one `User` bypass actor (ADR-1252). Do not relax
this to role tier. Do not restore unconditional zero-bypass assertion
without first removing that actor from live ruleset. Missing, truncated, or
error API data must fail. Checker only reads; CI must never gain
administration credentials or automatic apply path. Run its offline
adversarial controls after every change.

`hw_encoder_corpus.py` is a long-running, append-only corpus producer. It may
retain rows from successful quality points for diagnosis, but any encode,
decode, score, or canonical-row failure must make the process return non-zero.
Never turn a failed quality point into a successful partial corpus. Preserve
the positive, failed-encode, empty-metrics, and missing-input controls in
`tests/test_hw_encoder_corpus.py`.
