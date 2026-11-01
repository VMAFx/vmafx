<!-- markdownlint-disable MD041 -->
- **The tiny-model registry records each model's teacher.** `teacher` is a
  required field in `model/tiny/registry.schema.json`: a model id under
  `model/`, `none` (no VMAF teacher) or `unknown` (the tree holds no record of
  one). All 26 rows in `model/tiny/registry.json` carry it, and the training and
  export scripts write it.
