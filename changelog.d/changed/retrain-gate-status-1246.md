- `docs/ai/retrain-runbook-1246.md` gate table now reflects measured status rather than the
  state at authoring time. **G2** (master green) and **G3** (container rebuilt with the GPU
  default-model fixes) move to **PASS** — G3 verified on all four backends, not just CUDA —
  and **G1** and **G4** are sharpened with what specifically still blocks them. Each cell
  now records how and when it was checked.
