- `docs/ai/retrain-runbook-1246.md` gate table now reflects measured status rather than the
  state at authoring time. **G2** (master green) and **G3** (container rebuilt with the GPU
  default-model fixes) move to **PASS** — G3 verified on all four backends, not just CUDA —
  and **G1** and **G4** are sharpened with what specifically still blocks them. Each cell
  now records how and when it was checked.
- The runbook's K150K commands pointed `--scores` at
  `.corpus/konvid-150k/scores.csv`, which does not exist — KoNViD-150k splits its scores
  into `k150ka_scores.csv` / `k150kb_scores.csv`. The wrong path appeared in **both** the
  §4 five-clip smoke and the §5.1 multi-day production extraction, and
  `extract_k150k_features.py` fails closed on a missing file, so each would have aborted on
  its first line. Corrected to `k150ka_scores.csv`, which is also the script's own default.
