- **Live-window GPU texture test coverage on CUDA, SYCL, and HIP (#2238).**
  The OBS-ready live window harness (`core/test/test_vmafx_window_live.c`) verifies
  hardware texture import (GL textures with GL sync fences), asynchronous window scores
  matching offline sessions bit for bit, backpressure within bounded queues, and latency
  within the two-frame-period budget (<= 33.33 ms) across CUDA, SYCL, and HIP backends
  with zero host copies.
