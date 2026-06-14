Added the NIQE no-reference CPU feature extractor (`niqe`, ADR-1112):
a completely-blind, opinion-unaware image-quality metric that scores the
distorted picture only (no reference frame), reachable via `vmaf --feature
niqe`, the libvmaf C API, and the feature registry. The implementation
replicates the fork's Python harness byte-for-byte against the in-tree
pristine model `model/other_models/niqe_v0.1.pkl` (embedded as
`core/src/feature/niqe_model.h`), including the two load-bearing fork
divergences from upstream NIQE: the AGGD mean parameter's trailing
`*aggdratio` factor and the float32 round-trip of the MSCN maps and the
PIL-compatible bicubic half-resolution scale. Matches the harness at
places=4+ on natural content (`testdata/scores_cpu_niqe.json`). User docs in
`docs/metrics/niqe.md`; correctness gate in `core/test/test_niqe.c`. Lower
score is better; HDR and >8-bpc handling are documented limitations.
