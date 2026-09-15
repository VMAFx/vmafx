- **`scripts/dev/gc_workingdir.py` reclaims the rebuildable half of the local
  state trees and keeps the evidence.** The gitignored working directories
  accumulate one directory per gate run; on this workstation `.workingdir2/` had
  grown to **17 GB across 124,477 files**, of which only 570 were Markdown. The
  bulk was regenerable: Go build caches duplicated across runs, meson `build/`
  trees, compiled objects, downloaded envtest binaries, a rendered mkdocs
  `site/`. The evidence — receipts, help dumps, logs, CSV and JSON — was a small
  fraction. Committed documents cite those run directories by path, so the
  directories are part of the audit trail: the tool reads every citation out of
  the committed refs, keeps any path a document names, prunes the rebuildable
  content from underneath it, and writes a `GC-MANIFEST.md` into each pruned
  directory recording what went and when. Dry run by default. First run:
  **17 GB → 1.7 GB**, 224 citations honoured, 42 manifests written.
