- **MobileSal real-weights swap deferred (T6-2a-followup, ADR-0257)** —
  the original plan to swap the smoke-only `mobilesal_placeholder_v0`
  ONNX in `model/tiny/registry.json` for real upstream MobileSal
  weights (mirroring PR #326 / ADR-0253 for FastDVDnet) is deferred
  indefinitely. Survey in
  [`docs/research/0053-mobilesal-real-weights-blocker.md`](docs/research/0053-mobilesal-real-weights-blocker.md)
  shows three independent blockers: (1) upstream
  [`yuhuan-wu/MobileSal`](https://github.com/yuhuan-wu/MobileSal) is
  **CC BY-NC-SA 4.0** (incompatible with the fork's
  BSD-3-Clause-Plus-Patent — both the Non-Commercial and Share-Alike
  clauses bind), (2) trained checkpoints are distributed only via
  Google Drive viewer URLs (no GitHub release; no raw-download URL the
  export script can pin by SHA), and (3) MobileSal is RGB-D while the
  C-side contract is RGB-only. ADR-0218's claim that upstream MobileSal
  is "MIT-licensed" was inaccurate; corrected here and in
  [ADR-0257](docs/adr/0257-mobilesal-real-weights-deferred.md). The
  smoke-only placeholder remains shipped; the C-side
  `feature_mobilesal.c` extractor and its I/O contract are unchanged.
  `docs/ai/models/mobilesal.md` updated with the corrected upstream
  licence and the blocker pointer. Recommended replacement is to swap
  the underlying model family from MobileSal to U-2-Net's `u2netp`
  variant (Apache-2.0, 4.7 MB, pure RGB), tracked as new backlog row
  T6-2a-replace-with-u2netp; that scope shift is deliberately not
  bundled into this docs-only PR.
