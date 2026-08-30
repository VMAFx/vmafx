- CI merge throughput: the `Required Checks Aggregator` poll deadline is raised
  from 90 to 240 minutes (`timeout-minutes` 100 → 250), and Docker base-image
  digest/pin refreshes are now batched into a single Renovate PR
  (`groupName: "Docker digests"`). Together these break the starvation loop that
  kept every dependency PR unmergeable between 2026-06-29 and 2026-08-30: a
  ~44-PR Renovate backlog made the CI queue deeper than the 90-minute deadline,
  so aggregators expired with siblings still `queued` and **no real check
  failure** — and because the aggregator is a required check, the expiry blocked
  the merge, which kept the queue deep. Six `[SECURITY]` updates were stuck
  behind it, including the repository's only CRITICAL advisory. Docker digests
  were the one dependency class with no grouping rule, so ~11 images each opened
  their own PR and their own full CI matrix. See ADR-1123.
