- The FFmpeg `libvmaf_cuda` filter no longer returns a different pooled VMAF from run to
  run. With `VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE` the caller copies frames into a
  libvmaf-owned device picture on its own stream, and libvmaf records a picture's `ready`
  event only when libvmaf itself performs the upload — so in that hand-over path nothing
  ordered the CUDA kernels against the producer's write, and one frame per run came back
  with corrupted ADM features. libvmaf now orders the frame once, at the CUDA dispatch
  point, before any extractor reads it: 56 of 60 runs corrupted before, 0 of 60 after,
  measured interleaved under concurrent CUDA load, at no measurable throughput cost. See
  [ADR-1199](docs/adr/1199-cuda-picture-handover-barrier.md).
