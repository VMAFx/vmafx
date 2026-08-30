- **docs**: ADR `Proposed` → final-status sweep covering the 26 ADRs
  whose front-matter still carried `**Status**: Proposed` at HEAD on
  2026-05-08 (deliberate exclusion: ADR-0325, contested by in-flight
  PRs and scoped to the merge-train renumber sweep). Companion to
  `changelog.d/changed/adr-bulk-status-flip-2026-05-06.md` which
  flipped the prior 13. **Accepted**: ADR-0125 / 0126 / 0127 / 0129
  / 0138 / 0139 / 0140 / 0207 / 0208 / 0235 / 0238 / 0239 / 0251 /
  0253 / 0270 / 0272 / 0276 / 0279 / 0295 / 0314 (also resolves the
  unresolved Git conflict markers around its Status line that a
  rebase reintroduced after the 2026-05-06 sweep) / 0315 / 0324.
  **Stay Proposed** (work in flight, gap documented in the appendix):
  ADR-0128 (MCP runtime is `-ENOSYS` stub awaiting T5-2b; ADR-0209
  audit-first scaffold is Accepted but the transports remain unwired)
  and ADR-0236 (DISTS extractor — T7-DISTS not started). Per
  ADR-0028 / ADR-0106 immutability rule, each ADR's original body is
  unchanged; status flips land as a `### Status update 2026-05-08`
  appendix that records the verification trail. Companion research
  digest: `docs/research/0086-adr-proposed-status-sweep-2026-05-08.md`.
