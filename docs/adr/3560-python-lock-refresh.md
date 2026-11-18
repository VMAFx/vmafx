<!-- markdownlint-disable MD013 -->
# ADR-3560: Refresh Python dependency locks on Renovate branches

- **Status**: Proposed
- **Date**: 2026-11-18
- **Deciders**: Pending
- **Tags**: `ci`, `python`, `supply-chain`

## Context

The lock checker reads inputs, outputs and the pinned compiler version from `requirements/locks/manifest.json`. A changed input leaves its derived locks stale and fails `make python-locks-check`. The hosted Renovate app blocks repository post-upgrade commands; `renovate.json` ignores lock outputs.

## Decision

Propose a workflow on Renovate branch pushes affecting manifest inputs. It runs the existing lock checker write mode and publishes only manifest outputs as a signed-off release-bot commit. A shared publisher also serves the Intel lock workflow. The DCO exception requires the exact Python refresh subject, release-bot identity, Renovate opener, and only manifest output paths. This proposal awaits decision `vmafx-renovate-python-lock-refresh`; it must not land before that decision.

## Alternatives considered

| Alternative | Benefit | Cost |
| --- | --- | --- |
| Manual refresh on each pull request | No additional workflow | Maintainers must repair each affected pull request. |
| Renovate pip-compile manager | Renovate owns lock updates | Requires migration and verification of all manifest inputs, outputs and compiler settings. |

## Consequences

- Positive: Renovate input changes can receive refreshed locks without a manual commit.
- Negative: The release-bot requires a path-scoped DCO exception. Its single manifest entry uses Category D, 120 days, matching the Intel precedent; renew only while the hosted limitation remains, or remove it when no longer needed. An unreadable manifest or expired exception refuses the class.
- Neutral: An explicit lease refuses publication if the branch changes or is deleted during refresh, preserving its current state. The next branch push affecting a manifest input runs the refresh again; otherwise rerun it against the current tip. ADR-3555 records that `gitIgnoredAuthors` lets Renovate continue rebasing after the release-bot commit; this proposal relies on that existing configuration.

## References

- [ADR-3555](3555-intel-neo-lock-file.md): Intel lock refresh and Renovate author handling.
- [Python dependency locks](../development/python-dependency-locks.md).
