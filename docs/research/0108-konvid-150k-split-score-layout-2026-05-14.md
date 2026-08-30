<!-- markdownlint-disable MD025 MD060 -->
# Research 0108 — KonViD-150k Split Score Layout

# Research-0108

Date: 2026-05-14

## Question

The existing KonViD-150k adapter expected one `manifest.csv` with URLs,
MOS, optional standard deviation, and rating count. The local corpus
drop uses `k150ka_scores.csv` / `k150kb_scores.csv` plus
`k150ka_extracted/` / `k150kb_extracted/`, so the adapter could not
consume the corpus already staged under `.workingdir2/konvid-150k/`.

## Finding

The split score layout is a real upstream distribution shape, not a
test fixture. `k150ka_scores.csv` carries `video_name,video_score`;
`k150kb_scores.csv` carries `video_name,mos,video_score`. The staged
directories hold the matching MP4s, so no URL reconstruction is needed.

The MOS-corpus JSONL schema should not widen for this layout. The
shared trainers depend on the existing row shape, and split score CSVs
do not provide per-row standard deviation or rating counts. The correct
adapter behavior is to preserve `mos`, fill `mos_std_dev = 0.0`,
`n_ratings = 0`, and probe geometry from the local MP4s exactly as the
manifest path does.

## Alternatives Considered

| Option | Result | Reason |
|---|---|---|
| Require operators to synthesize `manifest.csv` manually | Rejected | Leaves the in-tree adapter unable to consume the common score-drop layout and repeats fragile local conversion logic. |
| Add separate `--split-score-layout` CLI mode | Rejected | The default directory has an unambiguous discovery order: use explicit/real `manifest.csv` first, otherwise discover split score CSVs. |
| Widen JSONL with `split` / score-source columns | Rejected | Downstream MOS-corpus consumers rely on the shared schema. Split identity is useful for diagnostics but not part of the trainer contract. |
| Auto-discover split score CSVs when `manifest.csv` is absent | Chosen | Consumes the staged corpus without schema drift, while explicit `--manifest-csv` remains strict so typoed paths fail loudly. |

## Verification

```bash
PYTHONPATH=ai/src .venv/bin/python -m pytest ai/tests/test_konvid_150k.py -q
.venv/bin/python -m ruff check ai/scripts/konvid_150k_to_corpus_jsonl.py ai/tests/test_konvid_150k.py
```
