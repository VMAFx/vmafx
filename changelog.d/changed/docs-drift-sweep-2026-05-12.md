### Documentation drift sweep — 2026-05-12

ADR index fragments, manifest, and surface-doc precision claims had drifted
from the implementation:

- **ADR index manifest reconciliation** (38 ADRs missing fragments, 73 ADRs
  missing from `docs/adr/_index_fragments/_order.txt`, 2 stale entries).
  Generated terse one-row fragments for every backfilled ADR using the
  ADR header (title + status + tags); appended missing slugs to the
  manifest in numeric order; regenerated `docs/adr/README.md` via
  `scripts/docs/concat-adr-index.sh --write`.
- **ADR-0411 (duplicate of ADR-0397)** deleted. Both ADRs shared the
  same `vmaf-tune` Phase F title and 197 lines of identical content;
  ADR-0397 has five additional status-update sections (F.1/F.2/F.4/F.5
  through 2026-05-10) so ADR-0411 was the strict subset. Removed the
  ADR file, its fragment, and the corresponding `_order.txt` entry.
- **Stale `_order.txt` entry `0297-vmaf-tune-sample-clip`** dropped —
  ADR was renumbered to `0301` during the 2026-05-02 collision sweep
  (commit `fb14bc33`) but `_order.txt` still pointed at the dead slug.
- **ADR-0415 status flipped from Proposed to Accepted** — the CAMBI
  SYCL port shipped as
  `core/src/feature/sycl/integer_cambi_sycl.cpp` (≈ 37 kB) so the
  Proposed marker was code-vs-doc drift; added a status-update
  paragraph pointing at the implementation file.
- **`--precision` default claim fixed in three places** (`AGENTS.md`,
  `CLAUDE.md`, `README.md`) plus the `python/vmaf/AGENTS.md` and
  `libvmaf/AGENTS.md` ADR citations. The default is `%.6f` (Netflix
  golden-gate compat per [ADR-0119](docs/adr/0119-cli-precision-default-revert.md),
  supersedes [ADR-0006](docs/adr/0006-cli-precision-17g-default.md));
  `--precision=max` opts in to `%.17g`. The previous text claimed the
  default was `%.17g` — the inverse of what `core/tools/cli_parse.c`
  actually does (`VMAF_DEFAULT_PRECISION_FMT = "%.6f"`,
  `VMAF_LOSSLESS_PRECISION_FMT = "%.17g"`).

No code, no test changes. ADR concat-gate `--check` exits clean post-sweep.
