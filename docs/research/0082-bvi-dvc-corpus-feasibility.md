<!-- markdownlint-disable MD060 -->
# Research-0082 — BVI-DVC corpus feasibility for `fr_regressor_v2`

## Question

Can adding the BVI-DVC reference corpus (Bristol VI Lab, 2021) to the
`fr_regressor_v2` training corpus alongside the Netflix Public drop
produce a measurable LOSO PLCC lift, given the license, content-overlap,
and fold-expansion posture the fork commits to?

## Method

1. Audit the BVI-DVC license terms via the Bristol Visual Information
   Lab portal and Zenodo metadata; classify against fork redistribution
   rules.
2. Compare BVI-DVC's content distribution against the Netflix Public
   drop (9 sources) — qualitative content-class overlap (cinematic
   film_drama, sports, animation, wildlife, urban, texture).
3. Project the LOSO partition expansion: today the trainer holds out
   one of 9 sources per fold; with BVI-DVC's tier-D added, the
   partition expands to 9 + N source-folds.

## Result

### 1. License analysis

BVI-DVC is distributed under research-only terms — the corpus may be
used for research and may not be redistributed. The fork's existing
posture for the Netflix Public drop (ADR-0203) is the same model:
local-only handling, derived weights ship, source corpus does not.
The redistribution rule for BVI-DVC therefore requires no new
infrastructure; it requires the same gitignore + README discipline
already in place for `.workingdir2/netflix/`.

Concrete rules:

- `.workingdir2/BVI-DVC Part 1.zip` — gitignored.
- `.workingdir2/bvi-dvc-extracted/` — gitignored.
- `runs/full_features_bvi_dvc_*.parquet` — gitignored.
- `runs/bvi_dvc_corpus.jsonl` — gitignored.
- `~/.cache/vmaf-tiny-ai-bvi-dvc-full/` — outside repo, never committed.
- `model/tiny/fr_regressor_v2*.onnx` — derived weights, **shippable**.

### 2. Content overlap with Netflix Public

| Content class       | Netflix sources                      | BVI-DVC tier-D coverage |
|---------------------|--------------------------------------|--------------------------|
| film_drama          | Seeking, ElFuente1/2, OldTownCross   | Sparse                   |
| sports / high-motion| CrowdRun, Tennis                     | **Strong** (e.g. tai-chi, walking, market scenes) |
| animation           | BigBuckBunny                         | None                     |
| wildlife            | BirdsInCage, FoxBird                 | Some (ferris-wheel, hong-kong-market) |
| urban / architectural | None                               | **Strong** (street, rooftop, canal) |
| texture-heavy       | None                                 | **Strong** (BVITexture clips) |

The Netflix drop under-represents urban and texture-heavy content;
BVI-DVC fills both gaps. Animation has no BVI-DVC analogue, so
BigBuckBunny remains the only anchor for that class.

### 3. LOSO partition expansion

Netflix-only LOSO trains 9 models, one per held-out source. Adding
BVI-DVC tier-D (~120 sources) expands the partition to 9 + ~120
source-folds. Each fold trains on a much larger remainder set, which
should narrow the per-fold variance band.

Trade-off: tier-D's resolution (480 × 272) is well below typical
production resolutions, so weighting BVI-DVC tier-D folds equally
with Netflix HD/UHD folds may bias the regressor toward low-res
behaviour. Two mitigations are available:

- include tier-B (1920 × 1088) and tier-C (960 × 544) instead of /
  alongside tier-D — increases extraction cost ~16× per clip;
- weight folds by source-resolution category in the LOSO aggregator —
  changes only the reporting layer.

The ingestion ADR-0310 ships the infrastructure tier-agnostic; tier
selection is a runtime flag, deferred to the multi-seed sweep.

## Conclusion

License is compatible with the fork's existing local-only corpus
posture. Content overlap is favourable: BVI-DVC fills the urban and
texture gaps the Netflix drop has and reinforces high-motion. The
LOSO partition expansion materially widens the training surface. The
infrastructure to make this measurable (JSONL adapter + merge utility

- tests) is small and ships with ADR-0310.

The actual PLCC measurement and the production-weights flip are
deferred to a multi-seed sweep that runs outside this PR. The flip
gate stays anchored on ADR-0303's ensemble criterion; a corpus
expansion that does not lift mean LOSO PLCC by ≥ 1σ above the
Netflix-only baseline is not shipped to production weights.

## References

- Ma, Zhang, Bull. *BVI-DVC: A Training Database for Deep Video
  Compression*. IEEE Transactions on Multimedia, 2021.
- [ADR-0203](../adr/0203-netflix-corpus-training-stack.md) — Netflix
  Public drop redistribution posture.
- [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) — `fr_regressor_v2`.
- [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-flip.md) — ensemble-flip
  ship gate (corpus-expansion ship criterion lives here).
- [ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md) — this digest's
  decision record.
