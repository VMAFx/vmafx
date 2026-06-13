### vmaf-tune: `--neg` flag for VMAF NEG model variants (ADR-0622)

Add a `--neg` flag to `vmaf-tune recommend`, `compare`, `tune-per-shot`,
`ladder`, and `corpus`. When set, the flag routes the VMAF model to the
No Enhancement Gain (NEG) variant (`vmaf_v0.6.1neg` / `vmaf_4k_v0.6.1neg`),
which is resistant to sharpening-based score inflation and appropriate for
codec A vs B comparisons. Model files were already in-tree; this is pure
parameter plumbing (ADR-0616 Option A). See `docs/metrics/vmaf-neg.md` for
when to use NEG and when not to.
