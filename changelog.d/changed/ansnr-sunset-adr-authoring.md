# Author dedicated ADR-0865 for ANSNR sunset (closes ADR-0108 compliance gap)

PR #38 (2026-05-28) removed `float_ansnr` from the C backend, citing a
`Parent ADR-0709` in its body. That cite was incorrect: ADR-0709 is the
VMAFX Phase 4b distributed-platform umbrella and contains zero ANSNR
content. No dedicated ANSNR-sunset ADR existed in tree. PR #295 and
PR #324 inherited the bad cite.

This change authors [ADR-0865](docs/adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md)
back-dated to 2026-05-28 (PR #38 merge date) so the ANSNR-drop decision
has a real parent ADR. The ADR documents:

- Why ANSNR (a pre-VMAF 2001 metric) was safe to drop — Netflix never
  shipped it in any production VMAF model; Research-0733 confirmed zero
  feature importance.
- The three rejected alternatives (restore, keep-but-skip, no-ADR) with
  per-option pros/cons.
- The merge-history citation gap: PR #38's body cannot be rewritten, so
  future readers landing on PR #38 will see the wrong cite. ADR-0865's
  `## Notes` section is the recoverable backreference.

[ADR-0749](docs/adr/0749-sunset-legacy-vmaf-feature-extractor.md) (the
Python `VmafLegacyQualityRunner` sunset) already pointed at PR #38 as the
upstream cause; it now has a real parent ADR to chain through.

No code changes — docs-only.
