- **The upstream GStreamer `vmaf` element is checked against Netflix's libvmaf
  (#2237).** CI builds Netflix's libvmaf at the recorded parity pin and runs
  the upstream `vmaf` element of gst-plugins-bad on both libraries for every
  output format (JSON, XML, CSV) and every property case (features, pooling,
  subsampling, clipping, transform, phone model, confidence interval, threads,
  frame messages); the scores must be identical, and a planted-defect run must
  fail
  ([Upstream consumers](docs/development/upstream-consumers.md)).
