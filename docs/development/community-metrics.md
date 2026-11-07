# Community metrics

Issue [#2554](https://github.com/VMAFx/vmafx/issues/2554) (maintainer decision
Q-182 item d): response times and contribution flow are measured, not guessed.
`scripts/dev/community_metrics.py` produces the numbers; **output only** —
nothing is posted anywhere, the maintainer decides what to publish.

## Metrics

For a date window `--since` .. `--until` (inclusive):

| Metric | Definition |
| --- | --- |
| Opened / closed | Issues and pull requests created or closed inside the window (a pair both opened and closed inside the window is counted once, in both groups). |
| Maintainer first response | For every item **opened** in the window: the earliest comment or review authored by a maintainer — the `@logins` of [`MAINTAINERS.md`](../../MAINTAINERS.md) — that is not authored by a bot. Reported as the median and the 90th percentile in days, over the items that have one. An item with none is a stated `no response yet` row — never zero, never dropped. A response arriving after the item was closed still counts and its row is marked `post-close`. |
| Bots | Every login ending in `[bot]` seen in the window or the merged history. They are excluded from first responses and first merges, and the report lists the logins it treated as bots so the rule is auditable. |
| Open `good first issue` / `help wanted` | Current open counts for the two labels (a snapshot, not window-scoped). |
| Aging pull requests | Open pull requests older than 30 days, with their age. |
| First merged pull request | Authors whose **earliest merge** falls inside the window, derived from the whole merged history — never from `author_association`, which GitHub computes at query time (a merged pull request's author always reads `CONTRIBUTOR`; measured on #1082, 2026-10-21). Bots and maintainers are excluded. The merged-history fetch fails when it reaches its page limit instead of truncating, because a truncated history would misreport exactly this number. |

## Running it

```bash
python3 scripts/dev/community_metrics.py --since 2026-10-01 [--until 2026-10-31] [--format json]
```

`--repo OWNER/REPO` overrides the repository (default: the `gh` current
repository). The script reads the GitHub API through the `gh` CLI — no token
handling of its own — and prints the report to stdout in Markdown (default) or
JSON.

## Tests

The fixture suite runs the whole pipeline on recorded API response shapes with
no network:

```bash
python3 -m unittest scripts.dev.tests.test_community_metrics
```

The fixtures cover the acceptance cases of issue #2554: expected medians and
counts, an item with no maintainer response (stated, never zero), an empty
window, a single item, an answer arriving after close, the first-merge
derivation ignoring `author_association`, and the merged-history limit failing
instead of truncating.
