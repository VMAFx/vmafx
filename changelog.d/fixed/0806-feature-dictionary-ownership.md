- **fix(api):** eliminate double-free (CWE-415) and memory leak (CWE-401) in
  `VmafFeatureDictionary` test callers.  `vmaf_use_feature()` and
  `vmaf_model_feature_overload()` unconditionally take ownership of the dictionary
  argument; `test_vif_skip_scale0.c` was calling `vmaf_feature_dictionary_free()`
  again on an error path (double-free), and `test_integer_vif_cpu_cuda_parity.c`
  leaked the dict when no CUDA device was present and the helper returned before
  reaching `vmaf_use_feature()`.  Ownership contract codified in ADR-0806.
