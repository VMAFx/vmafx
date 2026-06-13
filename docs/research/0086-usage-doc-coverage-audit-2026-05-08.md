# Research-0086: Usage-doc coverage audit against ADR-shipped surfaces

- **Status**: Active
- **Workstream**: ADR-0100 (project-wide doc-substance rule),
  ADR-0167 (doc-drift enforcement)
- **Last updated**: 2026-05-08

## Question

Which ADR-shipped user-discoverable surfaces ship without a corresponding
human-readable doc under `docs/usage/`, `docs/api/`, `docs/metrics/`,
`docs/backends/`, `docs/ai/`, or `docs/mcp/`, in violation of CLAUDE.md
§12 r10? Per ADR-0100 every user-facing surface should land its
substance-doc in the same PR as the code; pre-rule and rapid-shipping
surfaces have accumulated drift. Which gaps are highest-leverage to
backfill first?

## Sources

- [ADR-0100 — project-wide doc-substance rule](../adr/0100-project-wide-doc-substance-rule.md)
  — defines the per-surface minimum bars.
- [ADR-0042 — tiny-AI 5-pt doc bar](../adr/0042-tinyai-docs-required-per-pr.md)
  — tighter specialisation for the tiny-AI surface.
- [ADR-0167 — doc-drift enforcement](../adr/0167-doc-drift-enforcement.md)
  — gate that fails CI on documented drift.
- The 255 ADR files under [`docs/adr/`](../adr/README.md) (status pulled from
  the YAML-style header).
- Existing topic-tree doc inventory under `docs/{usage,api,metrics,backends,ai,mcp}`
  (108 files at the time of audit, excluding ADR / research / state.md).
- [ADR-0028 — ADR maintenance rule](../adr/0028-adr-maintenance-rule.md)
  — body immutability; informs that the audit cannot edit ADRs to
  patch the gap.

## Findings

### Audit method

1. Scanned every `docs/adr/NNNN-*.md`. Extracted `Status`, `Tags`,
   and the title.
2. Curated a list of ADRs whose Tags include any of `tooling`, `cli`,
   `ffmpeg`, `build`, `tiny-ai`, `mcp`, `metric`, plus the surface-tag
   extensions `extractor`, `backend`, `cuda`, `sycl`, `hip`, `vulkan`,
   `model`, `onnx` — the ADR families that, per CLAUDE.md §12 r10, are
   *required* to ship a user-discoverable surface.
3. Filtered out ADRs that are *decision-only* (process / CI / policy /
   audit / port-only / superseded / deferred) — these have no
   user-discoverable shape and the rule does not apply.
4. For each surface-bearing ADR, mapped the slug to the topic-tree doc
   that should host it, and looked up that path in the doc inventory.

### Counts

| Status                                                | Count |
|-------------------------------------------------------|------:|
| GOOD (doc exists)                                     |    46 |
| BACKFILL (missing doc)                                |    31 |
| N/A (decision-only / superseded / deferred / process) |   178 |
| **Total ADRs scanned**                                |   255 |

The 178 N/A items are pure-decision ADRs (Netflix-port records,
SIMD-bit-exactness records, internal CI / process records, GPU
kernel-impl records that surface only through `--feature` listings, etc.)
and do not require a per-ADR usage doc — they are catalogued in
`docs/metrics/features.md` or behind existing backend-overview pages.

### BACKFILL list (31 items)

Grouped by topic-tree target. Status flag in brackets.

**`docs/usage/` — vmaf-tune sub-surfaces (15)**

The Phase-A `vmaf-tune` doc covers the corpus + recommend baseline.
Subsequent ADRs each added a CLI flag or codec adapter; none shipped
a dedicated usage page.

- ADR-0298 `--cache` flag *(Accepted)*
- ADR-0299 `--score-backend=cuda|sycl` *(Accepted)*
- ADR-0314 `--score-backend=vulkan` *(Accepted)*
- ADR-0300 `--hdr` knobs *(Accepted, encode-side; HDR-VMAF deferred)*
- ADR-0301 `--sample-clip-seconds` *(Accepted)*
- ADR-0306 `--coarse-to-fine` *(Accepted)*
- ADR-0307 `--sampler` ladder default *(Accepted)*
- ADR-0289 `--resolution-aware` *(Accepted)*
- ADR-0293 `--saliency-aware` *(Accepted)*
- ADR-0297 multi-codec encode *(Accepted)*
- ADR-0304 fast-path prod wiring *(Accepted)*
- ADR-0295 Phase E ladder *(Proposed)*
- Codec adapters: ADR-0288 (x265), ADR-0290 (nvenc), ADR-0281 (qsv),
  ADR-0282 (amf), ADR-0283 (videotoolbox), ADR-0285 (vvenc-nnvc),
  ADR-0294 (svt-av1), ADR-0279 (libaom — note: filename collides with
  `0279-fr-regressor-v2-probabilistic.md`; libaom adapter content is
  housed in the codec-adapter ADR cluster).

**`docs/api/` — vmaf-tune-adjacent C-API (2)**

- ADR-0184 `vmaf_vulkan_import_image` scaffold *(Accepted)*
- ADR-0186 `vmaf_vulkan_import_image` impl + dmabuf path *(Accepted)*

**`docs/metrics/` — extractors (2)**

- ADR-0126 SSIMULACRA 2 extractor — *Proposed; mentioned in
  features.md but no dedicated page yet*.
- ADR-0236 DISTS extractor — *Proposed*.

**`docs/ai/` — tiny-AI (4)**

- ADR-0287 `vmaf_tiny_v5` corpus expansion — *deferred (no v5 model
  shipped); stub recording the deferral*.
- ADR-0324 ensemble training kit — *Proposed; Lawrence's hardware
  bundle*.
- ADR-0042 tiny-AI per-PR doc bar — meta/policy; usage-doc value is
  marginal but worth a stub for cross-link discoverability.
- (No standalone v3-mlp-medium / v4-mlp-large pages required —
  `vmaf_tiny_v3.md` / `vmaf_tiny_v4.md` model cards already cover.)

**`docs/mcp/` — MCP (1)**

- ADR-0166 MCP server release channel — *Accepted; build-channel
  surface (PyPI vs in-repo embedded)*.

**`docs/development/` — build / process (2)**

- ADR-0263 OSSF Scorecard policy *(Accepted)*.
- ADR-0277 ffmpeg-patches refresh process *(Accepted)*.

**`docs/usage/` — CLI fixes (1)**

- ADR-0316 `cli_parse` long-only error fix — *bug fix without
  user-visible flag delta; STUB stating "no usage delta" suffices*.

### Highest-leverage backfill picks (top 5)

The "leverage" metric used: **expected reader-traffic × surface
breadth × time-saving-on-discovery**. The top 5 are:

1. **vmaf-tune codec-adapter matrix** (8 ADRs collapsed into one page).
   Single most-referenced unfilled surface; users picking an encoder
   need a single matrix, not eight ADR-cross-links.
2. **vmaf-tune `--score-backend`** (ADR-0299 + ADR-0314). GPU acceleration
   of the score loop — a feature users actively look for; collapsing
   the two ADRs into one doc reflects how the flag is consumed.
3. **vmaf-tune `--cache`** (ADR-0298). Hits every `vmaf-tune corpus`
   re-run; pure user-time-saver flag; needs flag-shape + on-disk-shape
   doc.
4. **Vulkan image import** (ADR-0184 + ADR-0186). Public C-API entry
   point + ffmpeg integration; the only Vulkan zero-copy doc readers
   have today is the ADR, which is decision-prose, not API reference.
5. **vmaf-tune HDR + sample-clip** (ADR-0300 + ADR-0301). Combined
   page covers the encode-side HDR knobs and the per-clip sampling
   mode; users running HDR pipelines need both together.

The remaining BACKFILL items receive STUB pages — a one-paragraph
"this surface exists, see ADR-NNNN" placeholder so `grep`s and
mkdocs cross-links work; the full prose can land in a follow-up PR.

## Alternatives explored

- **Edit each governing ADR to inline the usage prose.** Rejected:
  ADR-0028 freezes the body of an Accepted ADR; the audit reports the
  gap, the docs are the fix.
- **Single mega-page `docs/usage/vmaf-tune-flags.md`.** Rejected:
  flag-density too high to keep navigable; the topic-tree convention
  is one tool / one matrix per page (see `vmaf-roi-score.md`,
  `bd-rate.md`, `bench.md` as precedents).
- **Auto-generate from `--help`.** Rejected for this PR: the help
  text doesn't carry the *why* / *when-to-use* prose that turns a
  flag into a usable feature; auto-gen is a follow-up to consider for
  the bench-matrix sub-pages once the prose surfaces stabilise.

## Open questions

- The BACKFILL stubs added by this PR all cite the relevant ADR. A
  follow-up sweep should flesh them out as full usage pages — track
  via the doc-drift-enforcement gate (ADR-0167) once the gate flags
  the stub hits as out-of-date.
- ADR-0042's tiny-AI 5-point bar applies to `docs/ai/` surfaces; this
  audit applies the looser ADR-0100 bar to non-AI surfaces. The two
  rules co-exist; no current surface is governed by both.
- ADR-0277 (ffmpeg-patches refresh process) is borderline N/A — the
  audit classified it as BACKFILL because it surfaces a `make refresh-ffmpeg-patches`
  operator workflow, but the per-surface bar may not actually require
  a separate page; downgrade-to-N/A is a defensible call. Stubbed as
  BACKFILL pending a reviewer call.

## Related

- ADRs: [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md),
  [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md),
  [ADR-0167](../adr/0167-doc-drift-enforcement.md),
  [ADR-0028](../adr/0028-adr-maintenance-rule.md).
- PRs: this audit PR; coordinates with #476 (ADR index backfill).
- Issues: none directly; ADR-0167 enforcement triggers will surface
  any stubs that go stale.
