---
paths:
  - scripts/ci/check-dco.py
  - scripts/ci/dco-cutoff.txt
  - scripts/ci/tests/test_check_dco.py
invariant: PR and landed commits refuse model/tool attribution; PR bodies scanned; sign-off and bot rules remain.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# check-dco.py invariants ([ADR-2462](../../../docs/adr/2462-dco-sign-off-required.md))

Gate: `Signed-off-by:` trailer on each non-merge PR commit; model/tool attribution refused.

1. **Trailer block only.** `trailer_lines()` asks `git interpret-trailers`; sign-off in prose
   not counted. Address = author or committer address.
2. **Bot exemption = two conditions.** PR author type `Bot` and login in `BOT_LOGINS` (from
   GitHub, not commit), plus commit author = bot noreply address. Commit
   author name alone attacker-controlled; never exempt on name. Keep `BOT_LOGINS`
   and `docs/development/dco.md` in step (`test_script_and_listed_bots_are_documented`).
3. **Unreadable range -> exit 2.** Never print PASS for range git failed to answer.
4. **Range = live target tip..head** in workflow (as `Silent-Revert Guard`), never
   `base.sha`.
5. **Release PR** exempt only through `release-pr-exempt.sh` (`DCO_RELEASE_PR`).
6. **Cutoff timestamp (`dco-cutoff.txt`)** grandfathers PRs created before rollout;
   missing creation time enforced; unreadable cutoff exits 2. Keep date in step with
   `CONTRIBUTING.md`.
7. **Attribution.** Commit author/committer name/email scanned before bot sign-off exemption;
   `[bot]` ignored for identities. Message lines and PR body lines share trailer/footer checks.
   `--pr-body-file` overrides `PR_BODY`; unreadable body file exits 2. Landed mode scans commits.
8. **Token sync.** `BANNED_COAUTHOR_TOKENS` = praetor `BANNED_COAUTHORS` + engine
   `bannedCoauthorsRegex`; change all three together.

Required context `DCO Sign-off` = job `dco-sign-off` in `rule-enforcement.yml`; one entry in
`required-aggregator.yml` list (`check-aggregator-names.sh`).
Regression: `python3 -B -m pytest -q -p no:cacheprovider -rs scripts/ci/tests/test_check_dco.py`.
