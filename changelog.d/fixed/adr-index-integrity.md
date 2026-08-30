- ADR index integrity restored at its source. `docs/adr/README.md` is generated
  from `docs/adr/_index_fragments/` by `scripts/docs/concat-adr-index.sh`, and
  the generator appends any fragment missing from `_order.txt` to the end of the
  table. Four fragments had been left behind for decisions that were renumbered
  out of a collision — `0452-vif-scratch-buf-hoist-to-vifstate`,
  `0460-ms-ssim-enable-db-clip-db-gpu-parity`,
  `0539-hip-ssimulacra2-blur-fp-contract-off` and `0567-upstream-port-direct-read`
  — whose ADR bodies now live at ADR-0578, ADR-0582, ADR-0594 and ADR-0600 (each
  already carrying its own fragment and `_order.txt` entry). Those four orphans
  are deleted. `0953-doxygen-public-api-clean` was listed twice in `_order.txt`;
  the duplicate is removed. Four legitimate fragments that were never added to
  `_order.txt` (ADR-0764, ADR-0866, ADR-0982, ADR-0993) are inserted in numeric
  position instead of being appended after ADR-1120. The regenerated index is
  878 rows: no duplicate numbers, every link resolves, and every numbered ADR has
  exactly one row.
