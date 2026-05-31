<!-- markdownlint-disable MD013 -->
# ADR-0100: Every user-discoverable change ships docs in the same PR

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: docs, agents, framework

## Context

[ADR-0042](0042-tinyai-docs-required-per-pr.md) mandated that every PR
touching the tiny-AI surface ships a human-readable doc page under
`docs/ai/` in the same PR. In practice the same failure mode exists
across the rest of the codebase: the fork is already complex (SYCL /
CUDA / HIP backends, SIMD paths, feature extractors, CLI flags, MCP
tools, a training CLI) and the tree keeps growing. A new contributor or
a downstream user should be able to *learn and use* a surface without
reading the implementation — that is the standing bar the user set:
"so that you can learn and use it if you dont know it".

This ADR is the first ADR written under the new-decisions-start-at-0100
rule from [ADR-0028](0028-adr-maintenance-rule.md).

## Decision

The doc-substance rule extends project-wide. **Every PR that adds or
changes something a user or contributor would plausibly need to learn
about ships a human-readable doc update under `docs/` in the same PR as
the code.** The minimum bar is *tailored per surface* so the rule stays
precise rather than generic; docs land in the existing `docs/` topic
trees and extend them as needed — no parallel `docs/surfaces/` tree.
The rule applies to additions and to behavior-visible modifications;
internal refactors, bug fixes with no user-visible delta, and test-only
changes are excluded.

### What counts as a "user-discoverable change"

Anything a user or contributor might need to learn to use it:

- New or changed **CLI flag** or CLI binary (`core/tools/`, `ai/`, `mcp-server/`).
- New or changed **public C API** (anything under `core/include/`).
- New or changed **feature extractor** (`core/src/feature/feature_*.c`).
- New or changed **GPU backend** or SIMD path (user-visible through
  backend selection or perf characteristics).
- New or changed **build flag** (`meson_options.txt`), including what
  dependencies it pulls in and what it gates at runtime.
- New or changed **ffmpeg filter** or filter option.
- New or changed **MCP tool** (`mcp-server/vmaf-mcp/`).
- New or changed **tiny-AI model / training recipe** — continues to
  follow [ADR-0042](0042-tinyai-docs-required-per-pr.md)'s tight 5-point
  bar.
- New or changed **error message, log format, or output schema** that a
  user might grep for or depend on.

### Per-surface minimum bars

| Surface | Minimum bar |
| --- | --- |
| **Tiny-AI (models, extractors, training CLI, MCP VLM tools)** | As [ADR-0042](0042-tinyai-docs-required-per-pr.md): (a) plain-English description, (b) output range + interpretation, (c) runnable copy-pasteable example, (d) checkpoint provenance + sha256, (e) known limitations. |
| **CLI flag / binary** (`vmaf`, `vmaf_bench`, `vmaf-train`, future `vmaf-roi`, `vmaf-perShot`) | (a) what it does, (b) arguments + defaults, (c) runnable example producing concrete output, (d) how the output surfaces (stderr / file format / exit code), (e) interaction with other flags or known limitations. |
| **Public C API** (additions / changes under `core/include/`) | (a) what the function / type does, (b) inputs + outputs, including ownership + lifetime, (c) thread-safety note, (d) ABI stability tag (stable / experimental), (e) runnable C snippet, (f) error semantics (return codes / errno / `VMAF_ERR_*`). |
| **Feature extractor** | (a) what the feature measures in plain English, (b) numeric range + how to interpret it, (c) invocation example via `--feature=` or C API, (d) supported input formats (bit-depth, chroma subsampling, color space), (e) known limitations vs the reference paper or upstream implementation. |
| **GPU backend / SIMD path** | (a) what it enables and the prerequisites (toolchain, SDK version), (b) how to build with it (`meson setup` line), (c) how to invoke (backend selection flag or feature name), (d) numerical agreement vs CPU reference (ULP budget), (e) known gaps (which features not yet ported). |
| **Build flag** (`meson_options.txt`) | (a) what it enables, (b) default, (c) dependencies pulled in, (d) runtime effect (fatal if missing at runtime? graceful fallback? feature simply unavailable?). |
| **MCP tool** | (a) what it does, (b) JSON-RPC input schema, (c) example call + response, (d) side effects (filesystem writes, subprocess launches, network calls), (e) authorization / sandboxing notes. |
| **Output schema / error / log format change** | (a) what changed, (b) before/after example, (c) how to migrate consumers, (d) deprecation timeline if anything is removed. |

### Where docs live

Extend the existing topic trees under `docs/` ([ADR-0031](0031-fork-docs-moved-under-docs.md)):

- CLI flags, binaries, invocation → [`docs/usage/`](../usage/)
- Public C API → [`docs/api/`](../api/) (create if absent)
- Feature extractors / metrics → [`docs/metrics/`](../metrics/)
- GPU backends / SIMD → [`docs/backends/`](../backends/)
- Build system, contributing, release → [`docs/development/`](../development/)
- Tiny-AI → [`docs/ai/`](../ai/)
- MCP server → new [`docs/mcp/`](../mcp/) when it has more than one page's worth of content
- Architecture / repo map / C4 → [`docs/architecture/`](../architecture/)

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep rule scoped to tiny-AI (ADR-0042 only) | Smallest footprint; already enforced | Rest of the codebase accumulates doc debt silently; a user wanting to adopt the CLI or C API today has to read source | User rejected ("document everything, vmaf is complex enough") |
| One universal 5-point bar for every surface | Simplest enforcement | Loses precision — what "checkpoint provenance" means for a new SIMD path is nothing; the bar becomes aspirational | User chose tailored per-surface |
| Generic "ship docs" rule with no bar | Zero up-front design | Devolves to "I added one sentence" and the doc debt returns | Rejected |
| New `docs/surfaces/` tree, one subdir per surface | Rigid, obvious where things go | Duplicates the existing topic trees; breaks mkdocs nav continuity | User chose existing trees |
| Per-module colocated README.md files | Closest to the code | Hard to browse top-down; doesn't help the "learn and use it" case | Rejected |

Rationale: the rule needs to be precise enough that a reviewer can check
it ("did this PR add a feature extractor? if so, are the five items
below present?") without being so generic it's trivially satisfied.
Per-surface bars keep the precision. Existing topic trees keep
navigation consistent with what MkDocs already renders.

## Consequences

- **Positive**: a new user or contributor gets from `README.md` →
  `docs/` → a working invocation for any surface without reading source;
  doc debt stops accumulating silently; reviewers have a concrete
  checklist per surface; the `docs/ai/` precedent is normalised to the
  rest of the codebase.
- **Negative**: PRs that add new user-discoverable surfaces grow by the
  cost of a docs page (typically ~1 page of markdown + one code
  snippet); enforcement requires reviewer attention rather than an
  automated gate in v1.
- **Neutral / follow-ups**:
  - [CLAUDE.md](../../CLAUDE.md) §12 rule 10 rewritten to be the
    general project-wide rule, with ADR-0042 cited as the tiny-AI
    specialisation.
  - [AGENTS.md](../../AGENTS.md) §12 rule 7 mirrored.
  - [README.md](README.md) index updated with the ADR-0100 row.
  - Future work: a lightweight CI script that flags PRs whose diff
    touches specific path sets (e.g. `core/include/`,
    `meson_options.txt`, `mcp-server/`) without a corresponding `docs/`
    diff. Not blocking v1 — human review carries the rule for now.

## References

- Source: `req` (user, 2026-04-17: "okay new adr, the same docs
  decision of 0042 for the whole project... so document everything,
  vmaf is complex enough i guess, we will add a lot more so the docs
  dont hurt")
- Trigger-scope source: `req` (user, 2026-04-17 popup answer: "well i
  guess anything so that you can learn and use it if you dont know it")
- Predecessor ADRs: [ADR-0042](0042-tinyai-docs-required-per-pr.md) —
  tiny-AI specialisation, preserved verbatim.
- Related ADRs: [ADR-0031](0031-fork-docs-moved-under-docs.md) — fork
  docs under `docs/`; [ADR-0028](0028-adr-maintenance-rule.md) —
  ADR maintenance rule (this is the first ADR under the "start at
  0100" post-backfill policy).
