- **Regression test for model-collection growth failure.** When the array of a
  model collection cannot grow, `libvmaf` returns `-ENOMEM` and keeps the
  collection and its models intact; upstream Netflix/vmaf loses them
  (Netflix/vmaf PR #1590). The behaviour is unchanged, and
  `test_model_collection_growth` now holds it in place
  ([C API](docs/api/index.md)).
