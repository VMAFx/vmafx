- **chore(ci):** Audit `scripts/ci/coverage-check.sh` per-file critical
  overrides (ADR-0114). Tightens `core/src/dnn/tiny_extractor_template.h`
  from 10 % → 75 % after live coverage measured at 77.4 % (the original
  10 % cap was set when only one extractor instantiated the inline
  helpers; four now do). Locks 65 pp of de-facto regression-coverage
  that the override was silently giving up. Other overrides
  (`ort_backend.c` 78 %, `dnn_api.c` 78 %) kept at cap — both files hug
  the threshold per ADR-0114's structural-ceiling rationale. No new
  override entries required: `dnn_attach_api.c` (92 %), `model_loader.c`
  (87 %), `onnx_scan.c` (93 %), `op_allowlist.c` (100 %), `tensor_io.c`
  (98 %), `opt.c` (100 %), `read_json_model.c` (88 %) all clear the
  global 85 % critical floor. (ADR-0881)
