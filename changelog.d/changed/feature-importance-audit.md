### Feature importance audit — Research-0733

Data-driven permutation importance audit of all feature extractors against the
shipped tiny-AI models (vmaf_tiny_v2/v3/v4) and KoNViD-1k MOS ground truth.
Confirms the canonical-6 (adm2, vif_scale0..3, motion2) account for 100% of model
predictive weight. Identifies `ansnr` and the `speed_temporal` / `speed_chroma`
bundle (~7,409 LOC across backends) as drop candidates with zero model weight and
low standalone MOS correlation. Also surfaces a path-resolution bug in
`scripts/dev/permutation_importance.py` when invoked from a git worktree.
No code changes in this PR — research only.
