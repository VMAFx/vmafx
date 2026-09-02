### Cross-backend throughput baseline + SYCL on Intel Arc (Research-0734)

Extended the PR #75 CPU/CUDA baseline with measured SYCL (Intel Arc A380) numbers.
Fixed the one-off container SYCL device-access issue: `docker run` with `--device /dev/dri`
does not pass `/dev/dri/by-path` symlinks, which the Level Zero GPU ICD requires to enumerate
Intel devices. The fix is `-v /dev/dri/by-path:/dev/dri/by-path:ro` on the `docker run` command.
Additionally, `--group-add render` fails inside the container; use `--group-add 988` (render GID).

Results (576×324, 48 frames, vmaf_v0.6.1): CPU 85 ms / 565 fps, SYCL 83 ms / 578 fps,
CUDA 139 ms / 345 fps. SYCL scores are bit-identical to CPU (Δ = 0) on all three workloads.
CUDA divergence is 3–4e-6, well within ADR-0119 tolerance.
