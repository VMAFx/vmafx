- Every score now carries a full provenance record (#2142, ADR-2073): library,
  ABI and build (commit, compilers, build options, the strict floating-point
  policy, backends, a `build_id` digest), the SIMD level and device, the
  context options, the frames, every model with the SHA-256 of its bytes and
  its overrides, the extractor, options, backend and exactness class of every
  feature (option-decorated names included), the command line, and a digest
  over the record and every score's bits. The C API reads it with
  `vmafx_context_provenance()`, `vmafx_context_model_provenance()`,
  `vmafx_context_feature_provenance()`, `vmafx_feature_provenance()`,
  `vmafx_context_annotation()` and `vmafx_context_provenance_json()`, and adds
  to it with `vmafx_context_annotate()` and `vmafx_context_set_encode_record()`
  (the digest of a VMAFx/pelorus#81 encode record). ABI 0.1.5.
- `vmafx_report_write()` writes a report with the record: a `provenance`
  object and `score_format` in JSON, a `<provenance>` element in XML, CSV and
  SUB unchanged with an optional `<path>.provenance.json` sidecar
  (`vmaf --provenance-sidecar`). Every `vmaf` report and every report an API
  user writes through `vmaf_write_output()` carries it; the scoring server and
  both MCP servers return it.
- `vmaf --verify-provenance <report>` re-runs the command line a JSON report
  recorded and compares every configuration field and score bit for bit,
  naming the first difference (exit 0 match, 1 difference, 2 cannot check);
  `vmafx_report_open()`, `vmafx_report_field()` and `vmafx_report_verify()`
  do the same for API users. Documented in `docs/usage/provenance.md`.
