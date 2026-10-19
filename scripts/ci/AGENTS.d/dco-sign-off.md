---
paths:
  - scripts/ci/check-dco.py
  - scripts/ci/dco-cutoff.txt
  - scripts/ci/tests/test_check_dco.py
invariant: Non-merge PR commits require author/committer sign-off; bot exemption two-sided; fail closed.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# check-dco.py invariants ([ADR-2462](../../../docs/adr/2462-dco-sign-off-required.md))

Gate requires `Signed-off-by:` trailer on every non-merge commit of pull request.

1. **Trailer block only.** `trailer_lines()` asks `git interpret-trailers`; sign-off in prose
   does not count. Address must equal author or committer address.
2. **Bot exemption is two conditions.** PR author type `Bot` and login in `BOT_LOGINS` (from
   GitHub, not commit), and commit author is bot noreply address. Commit
   author name alone is attacker-controlled; never exempt on it. Keep `BOT_LOGINS`
   and `docs/development/dco.md` in step (`test_script_and_listed_bots_are_documented`).
3. **Unreadable range is exit 2.** Never print PASS for range git could not answer.
4. **Range = live target tip..head** in workflow (as `Silent-Revert Guard`), never
   `base.sha`.
5. **Release PR** is exempt only through `release-pr-exempt.sh` (`DCO_RELEASE_PR`).
6. **Cutoff timestamp (`dco-cutoff.txt`)** grandfathers PRs created before rollout;
   missing creation time enforced; unreadable cutoff exits 2. Keep date in step with
   `CONTRIBUTING.md`.

Required context `DCO Sign-off` = job `dco-sign-off` in `rule-enforcement.yml`; one entry in
`required-aggregator.yml` list (`check-aggregator-names.sh`).
Regression: `python3 -m unittest scripts.ci.tests.test_check_dco`.
