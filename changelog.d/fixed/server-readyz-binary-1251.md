- `vmafx-server` `GET /readyz` now returns 503 when the vmaf binary the scorer
  runs has been removed, is no longer executable, or when `model.dir` is not a
  directory, instead of staying 200 for as long as the process holds a scorer
  object. The same check is a readiness check (`vmaf-binary`) on the golusoris
  status registry (#1251).
