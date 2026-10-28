- Tests: `test_picture_convert_log` installs its capture sink from static storage, so the
  thread-local sink pointer never holds the address of a returned stack frame (CodeQL
  `cpp/stack-address-escape`, `core/src/log.cpp`). Four new cases cover
  `vmaf_conversion_state_register_model()`: a model whose conversion target differs from the
  registered one is refused with `-EINVAL` and one ERROR line naming it, an equal target is
  accepted.
