- **Dedup duplicate-NNNN ADRs (bookkeeping).** Renumbered ten ADR files
  that violated the `docs/adr/README.md` "IDs assigned in commit order
  and never reused" rule (5 NNNN values had 2–7 sharing files;
  earliest-committed file at each colliding NNNN kept its number, the
  rest moved into the next free range 0242–0251). Filenames, H1
  headings, in-tree citations (`docs/`, `core/src/`, `ai/`,
  `scripts/`, `model/`, `mcp-server/`), and `docs/adr/README.md` index
  rows updated; ADR body prose is unchanged. Mappings recorded in
  [`docs/adr/README.md`](docs/adr/README.md) Conventions section under
  "2026-05-02 dedup sweep". Fork-private planning dossiers may still
  cite old NNNNs — consult the mapping table when resolving
  pre-sweep references.
