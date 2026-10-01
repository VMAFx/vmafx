- **`scripts/dev/speed_gpu_parity.py` accepts a relative `--vmaf` path.** The
  script refused every relative path, including its own default
  `build/tools/vmaf`, with `allowlisted executable must be bare or absolute`
  and exit status 2, so the commands in the guides and in `docs/state.md` did
  not run as written. A relative path is now taken from the working directory;
  an absolute path and a bare name on `PATH` work as before
  ([SpEED](docs/metrics/speed_qa.md#checking-a-gpu-twin-against-the-cpu)).
