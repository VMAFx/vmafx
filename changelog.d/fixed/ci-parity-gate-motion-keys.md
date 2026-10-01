- **Cross-backend parity gate compares emitted default motion metrics.**
  `scripts/ci/cross_backend_parity_gate.py` and `cross_backend_vif_diff.py`
  read `integer_motion`, which the CLI only emits in debug mode, causing a
  `KeyError` in the motion cell during default runs. Updated the motion metric
  keys to `integer_motion2` and `integer_motion3` so the full default parity matrix
  completes without error across backends. Also updated
  `scripts/ci/test_cross_backend_feature_names.py` to test active backends instead
  of the removed Vulkan backend.
