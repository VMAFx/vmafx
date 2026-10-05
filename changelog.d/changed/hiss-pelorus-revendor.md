- **The vendored Pelorus interop sources are re-vendored at a pin that carries
  the HISS splits.** `pel_blob_pack`, `pel_blob_find_section`,
  `pel_qp_report_from_blocks` and `pel_x265_csv_parse` are split upstream in
  `VMAFx/pelorus` and `core/src/interop/pelorus_*.c`, the Pelorus headers and
  the conformance fixture are re-rendered from that pin with
  `scripts/sync-pelorus-interop.sh --update` (ABI 1.3 unchanged, the
  conformance fixture passes). The pin also brings Pelorus's UTF-8 path opening
  for the qp-report CSV reader. The HISS baseline loses the nine rows the
  mirror carried.
