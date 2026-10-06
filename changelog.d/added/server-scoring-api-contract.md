- The scoring server's `ScoreRequest` (gRPC and `POST /v1/score`) takes
  `options`: raw `.yuv` geometry, backend and device, threads, subsample,
  precision, features, tiny model and more, so the server can score raw
  `.yuv` pairs. Every scoring response, the `ScoreStream` aggregate included,
  carries `provenance`: the library record, the model the server loaded with
  its SHA-256, the backend receipt and the precision. The versioned contract
  and its compatibility policy are on `docs/server/api-contract.md`; a
  contract test (`meson test test_vmafx_score_contract`) checks that the CLI,
  the C API, gRPC and REST give the same score bit for bit (#2155).
