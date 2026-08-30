- mkdocs strict build audit: remapped 22 dead ADR links across
  `docs/usage/`, `docs/metrics/`, and `docs/server/` to the
  surviving ADR slugs that took ownership of the original topics
  after ADR-number collisions reshuffled them (ADR-0386 /
  ADR-0535 / ADR-0628 allocator history). Edited
  `usage/vmaf-tune.md` (8 links: ADR-0549→0598, ADR-0291 slug,
  ADR-0370→0414, ADR-0298 slug, ADR-0294→ADR-0339,
  ADR-0222→per-shot-tool, ADR-0222→transnet-v2 shot detector,
  ADR-0279→0393, ADR-0325→0397), `metrics/vif.md` (3 links:
  ADR-0547→0597), `server/operator.md` (2 links:
  `kubebuilder-crds` → `operator-skeleton`),
  `usage/vmaf-tune-ladder.md` and `usage/vmaf-tune-recommend.md`
  (ADR-0279→0393), and `usage/vmaf-tune-fast-nr.md` (broken
  `vmaf-tune-per-shot.md` → in-page anchor in `vmaf-tune.md`).
  Strict build still exits 0; INFO drops from 2063 → 2041. The
  residual INFO tail is dominated by the two mkdocs.yml carveouts
  (source-tree pointers + immutable-ADR cross-references) per the
  `validation:` block comment.
