- **`motion_sycl` defaults `debug` to `false`, and the parity gate reports a
  missing metric as a cell error.** `motion_sycl` declared `debug` with default
  `true`, so a default run emitted `integer_motion` while the CPU, CUDA and HIP
  extractors did not; it now follows them (pass `debug=true` to get the score).
  `scripts/ci/cross_backend_parity_gate.py` and
  `scripts/ci/cross_backend_vif_diff.py` gain a `motion_debug` cell
  (`motion` with `debug=true`, comparing `integer_motion`, `integer_motion2`
  and `integer_motion3`). A metric that one backend does not emit now makes
  its cell `ERROR` and names the backend, instead of ending the whole matrix
  with `KeyError`. See
  [ADR-1418](docs/adr/1418-motion-parity-gate-metric-alignment.md)
  (`T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`).
