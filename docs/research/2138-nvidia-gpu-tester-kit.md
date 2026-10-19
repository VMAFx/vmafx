<!-- markdownlint-disable MD013 -->
# Research-2138: what an NVIDIA GPU tester image needs at run time, and what it may carry

- **Date**: 2026-10-03
- **Status**: input to [ADR-1509](../adr/1509-nvidia-gpu-tester-image.md)
- **Measured on**: RTX 4090 (compute capability 8.9) of a reference workstation, NVIDIA
  driver 615.71.09 (CUDA 13.4), NVIDIA Container Toolkit 1.20.0, Docker 29.8.2, in the
  image built from `docker/Dockerfile.tester --target final-cuda`.

## Question

Which NVIDIA files does a CUDA build of libvmaf need inside a container, which of them
may the project distribute, and which NVIDIA GPUs and access paths does one image
cover?

## Sources

- [CUDA Toolkit EULA](https://docs.nvidia.com/cuda/eula/index.html) v13.4, last updated
  2026-01-26 (read 2026-10-03), and the same text as the copyright file of every
  `cuda-*-13-4` package of NVIDIA's `debian13` repository (68 070 bytes).
- [CUDA 13.4 release notes](https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html),
  table 3 (minimum drivers).
- [NVIDIA Container Toolkit, specialized configurations](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/docker-specialized.html);
  [Docker Desktop GPU support](https://docs.docker.com/desktop/features/gpu/);
  [CUDA on WSL user guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html) v13.4.
- `core/src/meson.build` (gencode list, ADR-1223 floor), `core/src/cuda/common.c`
  (driver loading).

## Findings

1. **The binaries link no NVIDIA library.** `libvmaf` loads `libcuda.so.1` at run
   time through `nv-codec-headers`' `cuda_load_functions()` and embeds its kernels as
   fatbins (`bin2c`). In the build stage `readelf -d` of `vmaf`, `libvmaf.so` and all
   103 staged test executables lists no `libcuda*` or `libnv*` entry; the shipped stage
   has no file named like an NVIDIA library, and every binary resolves its libraries
   there (`ldd`). So the image needs none of the Attachment A runtime files.
2. **NVIDIA code is still inside the kernels.** nvcc inlines CUDA header code and links
   `libdevice.10.bc` functions (the fp64 maths of `ciede_cuda`, for example) into the
   device code. Attachment A lists `libdevice.10.bc`, the floating-point type headers
   and the runtime-compilation headers as distributable; 1.1.1(c) allows them
   incorporated in object code into an application meeting 1.1.2. The notices carry
   the EULA and pass on 1.1.2 and 1.2.
3. **The host driver arrives through the toolkit, read-only included.** The documented
   command (`--read-only --cap-drop ALL --security-opt no-new-privileges --tmpfs /tmp
   --gpus all`, uid 10001) reaches the GPU: the toolkit mounts the driver's libraries
   and the device nodes (`nvidiactl`, `nvidia0`, `nvidia-uvm`, `nvidia-uvm-tools`,
   `nvidia-modeset`, all readable and writable by the container's user). The CDI device
   `--device nvidia.com/gpu=all` gives the same result. `NVIDIA_DRIVER_CAPABILITIES`
   defaults to `utility,compute`; the image sets `compute,utility` explicitly.
4. **One gencode list covers every supported NVIDIA GPU.** The default build has cubins
   for `sm_80`, `sm_86`, `sm_89`, `sm_90`, `sm_100`, `sm_120` and PTX for `compute_80`
   and `compute_120` (nvcc 13.4.92 on Debian 13). A cubin runs on a later minor of its
   major, so 8.7 runs `sm_86`, 10.3 runs `sm_100` and 12.1 runs `sm_120`; 11.0 (Jetson
   Thor, an Arm machine the amd64 image cannot run anyway) and a future 13.x would JIT
   PTX. CUDA 13.x cubins load on drivers 580 and later; PTX of 13.4 needs a driver
   that knows it (R615). Turing and older are below the ADR-1223 floor; the report lists
   such a device with the reason and does not run it.
5. **The RTX 4090 run.** The documented command, `flock ... timeout 300`, exit 0 in
   2 min 22 s (host load average 5): verdict `pass`; 19 extractors on CUDA and 5 without
   a CUDA twin on the CPU (`brisque`, `delta_e_itp`, `niqe`, `pu21`, `y_funque_plus`);
   4332 values per run of the four fixtures, 0 differing from the CPU; the 19
   default-option gate features identical (2736 values), 6 option sets measured by the
   gate only; 98 gate cells OK at 0 and 2 SKIP (`float_ms_ssim_chroma` on the two
   576x324 pairs, chroma below its minimum); 66 device tests passed, 0 failed, 0
   skipped, 1 Python test left out (`test_cuda_parity_gate_default_run`). The Ada part
   of `T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03` passes with 111 items of evidence.
   The same with CDI. Without `--gpus all`: verdict `pass`, `gpu.status` `no_device`,
   reason "no NVIDIA GPU device node is visible: add --gpus all (NVIDIA Container
   Toolkit; with CDI: --device nvidia.com/gpu=all)". With one planted wrong reference
   value (`psnr_y` of frame 2 of the 576x324 pair, +1e-9): exit 1, verdict `fail`,
   `reference_equivalence` names the metric, frame 2 and both values.
6. **The licence gate fails closed.** On the exported image tree `licensing.py check
   --artifact cuda-image` passes; with a planted `libcudart.so.13` it reports "no
   recorded licence"; with the EULA text or the `nv-codec-headers` notice removed it
   reports the missing text (exit 1 each).
7. **Size.** 1.49 GB unpacked, 511 MB compressed content; 1.1 GB of it the device test
   executables, each linking `libvmaf` with its fatbins.

## Open questions

- WSL2: Docker Desktop passes the GPU as `/dev/dxg` with `/usr/lib/wsl/lib/libcuda.so.1`
  for `--gpus all`; whether the read-only, capability-free run as uid 10001 reaches it,
  and whether the twins' pinned host memory works under WSL2's limits, waits for the
  first report (`gpu.access.path` `wsl`).
- Ampere, Hopper and Blackwell: the measurements the kit exists for.
