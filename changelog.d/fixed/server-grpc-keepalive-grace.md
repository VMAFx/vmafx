- `vmafx-server` no longer cuts a `Score` or `ScoreStream` RPC that runs
  longer than about two minutes: the gRPC framework rotates connections after
  2 minutes with a 5 s grace for running RPCs, and the server now gives them
  30 minutes, the bound of one vmaf run (#1251).
