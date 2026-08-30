- **docs**: tighten `mkdocs build --strict` so it actually fails the
  docs CI lane on broken anchors, missing nav entries, and excluded
  fragment-tree leakage. Prior `mkdocs.yml` set every link-validation
  category to `info`, which silently drained `--strict` of teeth: the
  `.github/workflows/docs.yml` lane already passed `--strict` but had
  nothing to catch. Promotes `links.anchors` and `nav.{not_found,
  omitted_files}` to `warn`; keeps `links.{not_found,
  unrecognized_links}` at `info` because the dominant population is
  intentional cross-tree pointers from docs to source files
  (`../../core/src/...`, `../../scripts/ci/...`,
  `../../.github/workflows/...`) plus ADR-bodies whose target slug
  has been renamed — both unfixable under the existing fork policy
  (ADR-0028 / ADR-0106 immutability for the latter, no `index.md`
  for the former). Carve-outs documented inline in `mkdocs.yml`.
  Excludes `docs/adr/_index_fragments/**` from the rendered site
  (concatenation source per ADR-0221, never browseable). Fixes two
  pre-existing broken in-doc anchors (`docs/mcp/embedded.md` →
  ADR-0209's "What lands next" heading; `docs/research/0055-...md` →
  Research-0053's "Distribution" heading). Sweeps the small
  population of bare-relative-dir links (`backends/` →
  `backends/index.md`, `adr/` → `adr/README.md`) in
  `docs/index.md`, `docs/state.md`, `docs/rebase-notes.md`. Net:
  strict-build went from `EXIT=1` with 1276 emitted WARNINGs (after
  promoting validation to `warn`) to `EXIT=0` with the actionable
  classes still gated.
