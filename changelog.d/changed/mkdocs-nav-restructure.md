- **docs(nav):** Restructure mkdocs ADR section into per-hundred collapsible
  buckets (`0000-0099 — Foundation, build, golden gate` …
  `0700-0799 — VMAFx rebrand, Go/Rust/C++23, k8s`) plus an auto-generated
  by-tag sub-tree under `adr/by-tag/`. Adds two generator scripts —
  `scripts/docs/generate-adr-nav.sh` (splices the bucket block between
  sentinel comments in `mkdocs.yml`) and
  `scripts/docs/generate-adr-by-tag.sh` (scans every ADR's `Tags:` field
  and writes one `docs/adr/by-tag/<tag>.md` index per distinct tag plus a
  combined `index.md`). Both ship `--check` modes for CI drift detection.
  Removes the previous flat, unenumerated ADR tree behaviour where 600+
  files relied entirely on cross-link navigation from `adr/README.md`.
