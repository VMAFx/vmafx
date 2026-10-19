<!-- markdownlint-disable MD013 MD060 -->
# ADR-2689: sponsorship is recognition only, in four monthly tiers, with one source file for the list

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: maintainer, agent
- **Tags**: docs, community, funding, fork-local

## Context

The project's funding page listed only Ko-fi, and
[GOVERNANCE.md](../../GOVERNANCE.md) section 8 called GitHub Sponsors planned.
The GitHub Sponsors listing is now live on the maintainer's personal account.
A tier ladder, a statement of what sponsors receive, a currency policy and a
funding goal had to be fixed before the listing could be published, and the
repository needed one place that names sponsors. GitHub Sponsors takes US
dollars only; a euro audience needs another route.

## Decision

Sponsorship buys recognition only. There are four monthly tiers: Supporter
($5, name or handle in `SPONSORS.md`), Sustainer ($25, name and link in
`SPONSORS.md` and on the documentation site), Company sponsor ($100, logo in
the README and on the documentation site) and Lead sponsor ($500, top-placed
logo plus thanks in every release note); one-time $10 and $50 amounts are
thanked in `SPONSORS.md` for that month. There is no paid support, no feature
voting and no paywall. GitHub Sponsors carries the dollar amounts, Ko-fi and a
planned Patreon page the euro amounts (5, 25, 100 and 500 euros). The first
funding goal is $250 a month for CI and cloud GPU test time; an AI
workstation is the next goal. The repository root `SPONSORS.md` is the single
source of the tier table and the sponsor list; the documentation page
`docs/support-vmafx.md` includes both from it with `pymdownx.snippets`
instead of repeating them. The Patreon page is linked and added to
`.github/FUNDING.yml` only when it is live.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Recognition plus early access | Stronger incentive | Creates two classes of users; conflicts with open development | Recognition only keeps every release public at once |
| Recognition plus paid support | Revenue per company | Turns the maintainer into a vendor; obligations and response times | Not wanted; support stays the same for everyone |
| A $3 / $10 / $50 / $250 ladder | Lower entry | Low tiers cost the same effort to administer | The $5 / $25 / $100 / $500 ladder was chosen |
| Euro as the primary currency on GitHub | Matches the maintainer's currency | GitHub Sponsors takes USD only | Dollars on GitHub, euros on Ko-fi and Patreon |
| Copy the tier table into the docs page | Simple | Two lists drift apart | One source, included by snippet (HISS-19) |

## Consequences

- **Positive**: one public statement of what sponsorship is and is not; one
  file to update when a sponsor joins; the docs page cannot disagree with it.
- **Negative**: the docs build now depends on `pymdownx.snippets` reading a
  file outside `docs/`; a renamed marker in `SPONSORS.md` fails the strict
  build, which is the intended signal.
- **Neutral / follow-ups**: add the Patreon link and `patreon:` entry when the
  page is live; place logos by hand when a Company or Lead sponsor sends one.

## References

- Maintainer decisions Q-278 (tier ladder), Q-279 (recognition only), Q-280
  (Patreon page identity), Q-281 (who sets up the listing), Q-282 (currency),
  Q-283 (approval of the drafts), Q-284 (funding goal) and Q-285 (featured
  repositories), recorded in the maintainer's local decision log.
- [GOVERNANCE.md](../../GOVERNANCE.md) section 8, [SPONSORS.md](../../SPONSORS.md),
  [Support VMAFx](../support-vmafx.md).
