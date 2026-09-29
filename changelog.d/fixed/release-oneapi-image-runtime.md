- The oneAPI container image no longer crashes on Arc B580 (Battlemage)
  GPUs. Through v1.0.0-rc.2 it shipped the Intel GPU compute runtime of
  Intel's `oneapi-runtime:2025.3.1` image (version 25.18), and every
  `vmaf --backend sycl` run on a B580 ended with a segmentation fault (exit
  code 139) right after device selection. Arc A380 and UHD 770 GPUs were not
  affected. Replacing only that runtime with compute-runtime 26.35 stopped the
  crash; replacing only the Level Zero loader did not. The image now builds and
  runs on Debian 13, like the CPU image, with Intel's oneAPI 2026.1 compiler
  and SYCL runtime from Intel's apt repository at one exact build, and with the
  compute runtime (26.35.39758.10) and Level Zero loader (1.34.0) that the
  development container uses, all pinned in `build-config.env` (ADR-1368).
