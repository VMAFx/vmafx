<!-- markdownlint-disable MD060 -->
# ADR-0616: VMAF NEG Integration into vmaf-tune

- **Status**: Proposed
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ai`, `planning`, `vmaf-tune`, `docs`

## Context

VMAF NEG ("No Enhancement Gain") is a Netflix model variant that penalises
content where encoder in-loop sharpening inflates standard VMAF scores. The
NEG SVM model files are already present in `model/` (`vmaf_v0.6.1neg.json`,
`vmaf_4k_v0.6.1neg.json`, and float variants). However, `vmaf-tune`'s scoring
and bisect paths hardcode the standard model and expose no way to select NEG.
This makes `vmaf-tune compare` inappropriate for codec A vs B comparisons where
sharpening could mask compression differences.

## Decision

We will add a `--neg` flag to `vmaf-tune score`, `vmaf-tune bisect`,
`vmaf-tune per-shot`, and `vmaf-tune compare` (Research 0612 Option A). When
set, the flag routes to `model/vmaf_v0.6.1neg.json` (or the 4K variant when
`--model=4k`). This is pure parameter plumbing: no new code paths in libvmaf.
A follow-on Option D (auto-selection from the content classifier) is planned
post ADR-0618.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| A — `--neg` flag (chosen) | Minimal; clear intent; zero new code paths | Single-flag; does not auto-select on content type | Chosen as near-term simplest-win |
| B — `--model-variant` enum | Cleaner API; extensible | More surface to maintain; variant list may not be exhaustive | Deferred as V2 refactor |
| C — raw `--model` path in vmaf-tune | Zero model-specific code | Poor UX; no guard against unsupported formats | Chosen against; UX is poor |
| D — auto-selection from classifier | Fully automated | Requires ADR-0618 (content classifier) | Deferred post classifier |

## Consequences

- **Positive**: `vmaf-tune compare` becomes correct for codec A vs B
  evaluations; NEG is accessible without manual libvmaf CLI invocation.
- **Negative**: Users must explicitly pass `--neg`; wrong usage (e.g. `--neg`
  for production quality monitoring) produces misleading lower scores.
- **Neutral / follow-ups**: Documentation `docs/metrics/vmaf-neg.md` must
  explain when to use NEG and when not to; auto-selection (Option D) is the
  natural follow-on once ADR-0618 lands.

## Dependencies

- `model/vmaf_v0.6.1neg.json` — in-tree; no model training required.
- **ADR-0618** (content classifier) — optional follow-on for auto-selection.
- No other items in this 6-list are prerequisites.

## Implementation phases

| Phase | Description | Effort |
|-------|-------------|--------|
| P1 | `--neg` flag in CLI (`cli.py`); model path routing in `score_backend.py` | 0.5 day |
| P2 | Propagation through `bisect.py`, `per_shot.py`, `compare.py` | 0.5 day |
| P3 | Docs `docs/metrics/vmaf-neg.md`; "when to use" guidance | 0.5 day |
| P4 (post ADR-0618) | Auto-select NEG from classifier tag `animation-3d` | 0.5 day |

Total estimate: 1.5 days (the smallest-win item on the roadmap — model files exist).

## References

- Research digest: [docs/research/0612-vmaf-neg-integration-research.md](../research/0612-vmaf-neg-integration-research.md).
- Netflix upstream models doc (GitHub API 2026-05-19): `repos/Netflix/vmaf/contents/resource/doc/models.md`.
- Netflix Tech Blog "Toward a Better Quality Metric" (SSL failed 2026-05-19).
- `model/vmaf_v0.6.1neg.json`, `model/vmaf_4k_v0.6.1neg.json`.
- `tools/vmaf-tune/src/vmaftune/score_backend.py`, `bisect.py`.
- Source: per user direction (roadmap planning session 2026-05-19).
