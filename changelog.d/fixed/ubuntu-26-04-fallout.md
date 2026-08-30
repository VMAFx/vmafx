Container build unblocked for Ubuntu 26.04 (Resolute Raccoon): pin CUDA
`cuda-toolkit-13-2` from the ubuntu2404 NVIDIA apt repo (glibc 2.43 rsqrt
noexcept conflict resolved by CUDA 13.2); add `-D__MATH_NO_INLINES` to nvcc
flags in `meson.build` as a belt-and-suspenders guard; replace `python3.12`
packages with `python3.14` equivalents (3.12 not in Ubuntu 26.04 archive);
raise `requires-python` ceilings to `<3.15` in vmaf-tune and ai
pyproject.toml; rename `mesa-va-drivers` to `mesa-libgallium`; add
`libxml2-16` + `libxml2.so.2→libxml2.so.16` compat symlink to fix ROCm 7.2.3
LLD failing at HIP .hsaco link time; use ROCm `noble` channel (no `resolute`
channel yet); bump CI CUDA pin to 13.2.0 on all Jimver/cuda-toolkit-action
legs. (ADR-0603, triggered by Renovate PR #1402)
