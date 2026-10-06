- **VMAFx window scores and the window clock (RC4, ADR-1852, ADR-2074).**
  The VMAFx API gains asynchronous pooled scores over a range of frames:
  `vmafx_window_submit` takes a `VmafxWindowRequest` (a model, a model set or
  a feature, a set of pooling methods `VmafxPoolMask`, `first` / `last`, an
  optional callback) and returns at once; `vmafx_window_poll`,
  `vmafx_window_wait` and the callback deliver a `VmafxWindowResult` (one
  value per method, `n_frames`, `n_scored`, a partial flag) once every frame
  of the window is final, with the values of the synchronous pooled call bit
  for bit; `vmafx_window_release` cancels or frees it. Each context's
  completion thread finds completion as worker threads finish frames, so a
  window completes whether or not the producer calls again; callbacks run on
  a separate callback thread. `vmafx_flush` completes
  open windows over the frames the stream had. `VmafxWindowClock`
  (`vmafx_window_clock_create`, `_frame`, `_finish`, `_destroy`) cuts a stream
  into windows of `n_stats` seconds or `n_stats_frames` frames (#2138), and
  `vmafx_context_max_in_flight` reports the most frames a context holds after
  a submit (#2238). Windows over `motion2` / `motion3`, and so over VMAF
  models, complete at the flush in this release. ABI 0.1.8. See
  [window scores](docs/api/vmafx/windows.md).
