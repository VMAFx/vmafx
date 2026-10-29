- **The scoring contract test covers gRPC `ScoreStream` (#2155).**
  `test_vmafx_score_contract` now also streams the Netflix 576x324 pair
  through `ScoreStream` and requires its final `AggregateScore` to match the
  CLI, the C API, gRPC `Score` and `POST /v1/score` bit for bit; the contract
  page names the messages that carry no provenance and why
  ([Scoring API contract](docs/server/api-contract.md)).
