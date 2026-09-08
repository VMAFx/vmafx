**Complete Tiny-AI model cards and int8 fallback test coverage** (ADR-0042, ADR-1032, #1242)

- Added missing model cards for `smoke_multi_output_v0` and `smoke_v0_symbolic_batch` in `docs/ai/models/` and updated existing model cards (`vmaf_tiny_v1`, `vmaf_tiny_v1_medium`, `fr_regressor_v2`, `fr_regressor_v3`, `smoke_v0`, `smoke_fp16_v0`, `u2netp_mirror_card`) to satisfy the ADR-0042 5-point documentation standard (description, output range/interpretation, runnable example, provenance/facts, and known limitations).
- Added C unit test coverage in `core/test/dnn/test_dnn_session_api.c` and `core/test/dnn/test_vmaf_use_tiny_model.c` for the int8 session creation failure retry fallback to fp32 baseline, closing the CI coverage gap on `core/src/dnn/dnn_api.c`.
- Fixed test isolation in `ai/tests/test_measure_quant_drop_unit.py` to write test artifacts into temporary pytest directories rather than repo root paths.
