### ai: KoNViD / UGC / BVI-DVC saliency batch manifests

Add `ai/batch-manifests/saliency/` with in-tree batch manifests for the three
remaining corpora that have not yet had saliency features materialised (ADR-0993).
The KoNViD-150K manifest is fully wired and ready to run against
`.corpus/konvid-150k/konvid_150k.jsonl`. The UGC and BVI-DVC manifests are
scaffolded stubs that document the path-column blocking gap and both resolution
options (path-enriched JSONL generation vs feature-table re-extraction).
