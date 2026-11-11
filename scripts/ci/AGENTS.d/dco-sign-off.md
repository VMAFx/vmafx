---
paths:
  - scripts/ci/check-dco.py
  - scripts/ci/dco-cutoff.txt
  - scripts/ci/tests/check_dco_fixture.py
  - scripts/ci/tests/test_check_dco.py
  - scripts/ci/tests/test_check_dco_attribution.py
  - scripts/ci/tests/test_check_dco_landed.py
invariant: DCO attribution: NUL fields, bounded Unicode-normalized messages/bodies, event-file CI input; sign-off rules retained.
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
5. **Release PR** sign-off exempt only through `release-pr-exempt.sh` (`DCO_RELEASE_PR`); commit + body attribution still checked.
6. **Cutoff timestamp (`dco-cutoff.txt`)** grandfathers sign-off on PRs created before rollout; commit + body attribution still checked;
   missing creation time enforced; unreadable cutoff exits 2. Keep date in step with
   `CONTRIBUTING.md`.
7. **Attribution.** Commit author/committer name/email + message + PR body scanned on every PR (bot, release, cutoff sign-off exemptions included);
   `[bot]` ignored for identities. Message lines and PR body lines share trailer/footer checks.
   Lines + identities: shared NFKC normalization + `Cf` removal helper. Identities: every `Cf` removed, then lowercase substring tokens.
   Markdown quote/bullet (`-`, `*`, `+`)/comment/heading/ordered-list prefixes: shared linear stripper before co-author/footer checks; no exemption.
   Lines: prefix stripping first, then every remaining `Cf` anywhere removed before co-author/footer checks. Ordered marker: 1–9 ASCII digits + `.`/`)`, then whitespace/`Cf`/end; 10 digits -> prose unchanged.
   `--pr-body-file` (`--pr-body`) overrides local `PR_BODY`; CI `--pr-body-from-event` reads
   `GITHUB_EVENT_PATH` JSON. Unreadable/malformed file exits 2; null event body = empty.
   Named byte/line bounds: excess -> exit 1 with bound name, never truncated.
   Commit fields/records: NUL via `%x00` + `git log -z`; old separators preserved.
   One commit per bounded Git read; landed mode scans commits.
8. **Bounds.** `MAX_COMMIT_MESSAGE_BYTES/LINES` = measured master maxima ×4;
   `MAX_PR_BODY_BYTES/LINES` = PRs 2906-3395 (300 bodies) maxima ×4. JSON event envelope =
   body bytes ×6 + 1 MiB metadata. Boundary/+1 tests mandatory for every input bound.
9. **Token sync.** `BANNED_COAUTHOR_TOKENS` == praetor `BANNED_COAUTHORS`: all 13, substring matching.
   Engine `bannedCoauthorsRegex`: same 11 word tokens, word-boundary matching.
   E-mail + `[bot]` entries: Python-only. Token change -> update praetor tuple + local tuple;
   word-token change -> update engine regex word list together.

Required context `DCO Sign-off` = job `dco-sign-off` in `rule-enforcement.yml`; one entry in
`required-aggregator.yml` list (`check-aggregator-names.sh`).
Regression: `python3 -B -m pytest -q -p no:cacheprovider -rs scripts/ci/tests/test_check_dco.py scripts/ci/tests/test_check_dco_attribution.py scripts/ci/tests/test_check_dco_landed.py`.
