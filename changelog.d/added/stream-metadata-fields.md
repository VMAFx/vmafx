- **Stream metadata field list (ADR-3311).** `api/stream-metadata-fields.json`
  names the per-frame stream metadata fields VMAFx will report next to a score
  (coded size, average quantiser and its scale, picture type, key frame, codec,
  coded bit depth, display and decode index), spelled, typed and measured as
  the Pelorus telemetry field registry has them, and lists the registry's
  shared fields VMAFx does not report. The list is append-only and checked by
  a new tooling test; no output carries the fields yet
  ([Stream metadata field list](docs/api/stream-metadata-fields.md)).
