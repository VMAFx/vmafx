- **CI moved to the `ubuntu-26.04` runner image, and the jobs that staged
  gigabytes under `/tmp` were fixed to survive it.** On that image `/tmp` is a
  RAM-backed tmpfs with a per-user quota, so the Tiny AI job's virtualenv
  install died with `[Errno 122] Disk quota exceeded` while unpacking the torch
  and CUDA wheels; errno 122 is `EDQUOT`, which only a quota produces. The two
  virtualenvs, the ONNX Runtime archives and their cache, and the Kubernetes
  end-to-end workflow's buildx layer cache and image tar now use `RUNNER_TEMP`,
  which is on the runner's disk and emptied per job.
