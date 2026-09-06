- `docs/ai/retrain-runbook-1246.md` gate table now reflects measured status rather than the
  state at authoring time. **G2** (master green) and **G3** (container rebuilt with the GPU
  default-model fixes) move to **PASS** — G3 verified on all four backends, not just CUDA —
  and **G1** and **G4** are sharpened with what specifically still blocks them. Each cell
  now records how and when it was checked.
- The runbook's K150K commands were corrected against an actual run: `--scores` named a
  `scores.csv` that does not exist (KoNViD-150k splits scores into
  `k150ka_scores.csv` / `k150kb_scores.csv`), `--cpu-vmaf-bin` was missing entirely and its
  default path is absent from the container, and `--clips-dir` named `clips/`, whose
  153,841 entries are symlinks to *host* paths that do not resolve inside the container —
  every clip failed `ffprobe` in a way that reads like corrupt media but is a dangling
  link. All three appeared in **both** the §4 smoke and the §5.1 multi-day extraction, so
  each would have aborted. With them fixed the pipeline runs clean: `ok=5 fail=0` at
  1.26 clip/s. The three §4.2 assertions that still fail — `teacher_model` in the manifest,
  the `teacher_model` column, and `adm3_mean` — are all supplied by #1302, so G4 is blocked
  on that PR alone.
