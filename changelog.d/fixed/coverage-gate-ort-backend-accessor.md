- Coverage Gate on `master` was failing deterministically with
  `core/src/dnn/ort_backend.c` at 77.8 % (409 / 526 lines), 0.2 pp
  below the 78 % per-file floor ADR-0114 documents. The drift was
  introduced by PR #129 (`b8a51866e7`), which added 16 lines of
  correct-but-CI-unreachable `GetTensorElementType` error handling
  to `vmaf_ort_open` — the same structural-ceiling pattern ADR-0114
  catalogued for ADR-0113's `CreateSession→CPU` fallback. Rather
  than lower the per-file floor again, this fix lands a unit test
  in `core/test/dnn/test_ort_internals.c` that exercises the public
  accessor `vmaf_ort_output_name_at` (NULL-sess, OOB slot, happy
  path) — previously untested despite shipping to a production
  caller at `core/src/libvmaf.c:849` on the multi-output dispatch
  path. Recovers 4 measured lines of coverage, lifting the file to
  413 / 526 ≈ 78.5 % and clearing the gate with ~0.5 pp safety
  margin. ADR-0114's `PER_FILE_MIN["core/src/dnn/ort_backend.c"]=78`
  override stays in place.
