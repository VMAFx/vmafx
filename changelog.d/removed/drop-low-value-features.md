## Removed: ansnr / float_ansnr and speed_temporal / speed_chroma feature extractors (ADR-0716)

The `ansnr`, `float_ansnr`, `speed_temporal`, and `speed_chroma_{u,v,uv}` feature
extractors have been removed from the fork. Research-0733 (feature importance
audit, PR #25) established that both feature groups carry zero permutation importance
across all shipped tiny-AI models and exhibit near-zero standalone MOS correlation
(|PLCC| 0.06–0.17). All CPU, SIMD (AVX2/AVX-512/NEON), and GPU backend (CUDA, SYCL,
HIP, Vulkan, Metal) implementations are removed. Total reduction: approximately
7,400 LOC.

`--feature float_ansnr`, `--feature ansnr`, `--feature speed_temporal`, and
`--feature speed_chroma` now return an "unknown feature" error. The `speed_qa`
no-reference SpEED extractor is unaffected.

The `vmaf_v0.6.1.json` model and the Netflix golden-data gate are unaffected — that
model uses `adm2 + motion2 + vif_scale[0-3]` and does not reference ansnr or
speed_temporal/chroma.
