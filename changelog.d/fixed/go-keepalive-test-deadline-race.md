- Go tests: `TestScoringKeepaliveOutlastsFrameworkGrace` no longer fails at
  random on a loaded runner. Its stream helper sent the hold deadline to the
  server, whose health watch could end the stream first and look like a cut;
  the helper now cancels on a local timer
  ([state](docs/state.md#recently-closed)).
