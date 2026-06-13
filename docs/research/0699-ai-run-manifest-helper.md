# Research-0699: AI Run Manifest Helper

## Question

How much of the AI provenance boilerplate can be shared without flattening
script-specific report content into one vague schema?

## Findings

The ADR-0661 rollout left two valid patterns in tree:

- Stable reports such as validation metrics or training stats embed a
  `run_provenance` block into their existing schema.
- Standalone sidecars for local artifacts repeat the same outer envelope:
  `schema`, script-specific counters/configuration, and `run_provenance`.

Only the second pattern is duplicated enough to deserve a helper. The useful
per-script fields are not the same: fetchers record URLs and archive status,
feature extractors record row/pair/fail counts, GPU calibration records
backend/feature selections, and quantizers record byte ratios. Forcing those
fields into one schema would make reports less useful.

## Decision Drivers

- Keep report schemas stable for downstream readers.
- Remove repeated `schema` + `run_provenance` + deterministic-write boilerplate
  from standalone sidecars.
- Preserve adapter-style counters/config under script-owned keys.
- Put the pattern in `.claude/skills/` and `AGENTS.md` so future agent work
  does not reintroduce local manifest writers.

## Implementation Notes

`aiutils.run_manifest.write_run_manifest()` now builds a deterministic payload
with a script-specific `schema`, adapter `sections`, and shared
`run_provenance`. `build_run_manifest_payload()` exposes the same shape for
unit tests or callers that need the object before writing.

This batch converts the new legacy extractor/quantization sidecars through the
helper and adds `.claude/skills/ai-run-manifest/SKILL.md` as the operational
template.

## Follow-Up

Continue converting standalone sidecar writers when their scripts are touched.
Do not churn stable report files only to use the new helper; those should keep
embedding `build_run_provenance()` until a real schema change is needed.

Track a later per-tool sweep for `tools/vmaf-tune`, MCP, and dev-probe
helpers first. Most candidates are Python scripts, but their payload shape may
vary by package. Once two or three package-local helpers have settled, do a
cross-tool consolidation pass for the truly common pieces.

Because the repo has many artifact-producing scripts, the later sweep should
rank by duplication and user-facing impact before coding. Start with large
report/profile writers and repeated encoder/profile metadata paths, then leave
small one-off scripts until they are touched for functional work.
