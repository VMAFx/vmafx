<!-- markdownlint-disable MD013 MD060 -->
# ADR-1344: Leave `[project].version` out of the Python lock input fingerprint

- **Status**: Accepted
- **Date**: 2026-09-26
- **Deciders**: lusoris
- **Tags**: ci, release, dependencies, supply-chain

## Context

[ADR-1305](1305-hash-locked-python-installs.md) stamps every hash lock with `# vmafx-input-sha256:`, a fingerprint of its compile arguments and input files, and the required Pre-Commit check fails when an input changes without a regenerated lock. The fingerprint hashed each input byte for byte. release-please rewrites `version = "..."` in the `[project]` table of every released `pyproject.toml` (`ai/`, `dev-llm/`, `mcp-server/vmaf-mcp/`), so the 1.0.0-rc.1 release PR #1213 made seven locks "stale" although no dependency changed, and failed its required check. Every later release PR would fail the same way.

## Decision

We fingerprint a `pyproject.toml` input with the `version = ...` line of its `[project]` table removed. Every other byte still counts, including `version` keys in other tables, `[build-system]` and all dependency declarations; non-`pyproject.toml` inputs are hashed verbatim as before. The change lives in `fingerprint_content()` in `scripts/ci/check_python_dependency_locks.py`. The locks whose fingerprints change are restamped once with the reviewed uv release, seeded from their existing pins, so no locked version moves.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ignore `[project].version` in the fingerprint (chosen) | Release PRs pass unchanged; no bot writer; still byte-exact for everything that can affect resolution | The fingerprint is no longer a plain file hash | The one excluded line cannot change what the resolver picks |
| Workflow that restamps locks on release-please PRs | Keeps plain byte hashing | Adds a bot with push rights to release branches and a second commit author on every release | More moving parts and a new write path for a line that never matters |
| Restamp #1213 by hand | Unblocks the RC1 release today | Every later release PR breaks the same way | Treats a structural defect as a one-off |

## Consequences

- **Positive**: release-please PRs pass the required Pre-Commit check without manual lock edits. Overlaying #1213's version bumps on a branch with this change leaves all 26 locks valid.
- **Negative**: a lock no longer records the exact project version it was compiled against; the version is still visible in `pyproject.toml` itself.
- **Neutral / follow-ups**: if a project ever makes its own version resolution-relevant (for example a self-referencing extra), that input needs its version hashed again.

## References

- Popup, 2026-09-26: "Release PR #1213 (1.0.0-rc.1) fails the required Pre-Commit check: release-please bumps `version` in several pyproject.toml files, and the Python lock fingerprint hashes those files byte-for-byte, so 7 locks go 'stale' although no dependency changed. This recurs on every release PR. How should I fix it?" Answer: "Ignore [project].version (Recommended)".
- [ADR-1305](1305-hash-locked-python-installs.md) — hash-locked Python installs and the fingerprint contract.
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) — RC1 requires green required checks on the exact candidate head.
