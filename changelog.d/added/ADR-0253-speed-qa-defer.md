- **SpEED-QA feasibility digest + Proposed ADR (research-0051 / ADR-0253).**
  Closes the user's 2026-04-21 deep-research queued track on SpEED-QA as a
  candidate full-reference metric. Recommends DEFER over GO / SCAFFOLD-ONLY:
  the fork keeps the existing `speed_chroma` / `speed_temporal` research-stage
  extractors (PR #213, port of upstream `d3647c73`) and does not add a
  `speed_qa` reduction. Three findings drive the call —
  (1) SpEED-QA's GSM-entropy backbone overlaps `vif` substantially with no new
  perceptual axis; (2) the "10–40× faster than VIF" headline inverts on the
  fork's AVX-512 / CUDA / SYCL VIF stack; (3) the assumed-but-missing
  `model/speed_4_v0.6.0.json` upstream binary the brief referenced does not
  exist anywhere in `upstream/master`, `upstream/speed_ported`, or any open
  Netflix PR. Decision is reversible on three named triggers (see ADR-0253
  *Consequences → Follow-ups*). Docs-only PR — no code, no model registry
  change, no CLI flag, no behavioural delta. See
  [ADR-0253](docs/adr/0253-speed-qa-extractor.md) +
  [`docs/research/0051-speed-qa-feasibility.md`](docs/research/0051-speed-qa-feasibility.md).
