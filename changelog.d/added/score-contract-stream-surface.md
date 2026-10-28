- **The score contract test covers `ScoreStream` (#2155).** `meson test
  test_vmafx_score_contract` now also streams each frame pair through gRPC
  `ScoreStream` and requires its final `AggregateScore` to match the CLI, the
  C API, gRPC `Score` and `POST /v1/score` in score, library version and ABI;
  the contract page states which responses carry provenance
  ([Server API contract](docs/server/api-contract.md#responses-and-provenance)).
