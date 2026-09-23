- Repaired test build visibility seams and reachability contracts targeting 25
  open CodeQL `cpp/unused-static-function` findings. The audited span contains
  34 IDs (1066–1098 plus 1230), 33 selected findings, and nine pre-existing
  dismissals; hosted closure remains pending a fresh CodeQL run.
  Detached extraneous compilations of `thread_pool.c` and `pdjson.c` from test
  executables (`test_picture`, `test_picture_v2`, `test_predict`, `test_model`,
  `test_model_libsvm_dup_key`, `test_model_feature_overload_ownership`).
  Extended `test_pdjson_stack_increment.c` to exercise scalar tokens across
  string/stream/user sources and preallocated containers. Made `vector_unchanged`
  in `test_fex_ctx_vector.cpp` unconditionally reachable via dedicated positive
  and negative predicate assertions.
