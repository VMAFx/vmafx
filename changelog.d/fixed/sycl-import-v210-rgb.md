- **SYCL imports V210 and RGB frames like CUDA and HIP.** The V210 read
  and the RGB layouts added for CUDA and HIP never reached the SYCL
  backend: an imported V210 frame scored differently from the same frame
  uploaded from the host, and RGB imports had no conversion kernel. The
  SYCL gather now reads the grouped V210 form and a new kernel converts
  RGB with the host's integer plan (`rgb_math.h`), so every format cell
  scores bit for bit as the host frame on an Arc A380. The Vulkan fence
  test opens each frame's gate only after that frame was submitted, so a
  skipped acquire wait is caught on slower devices as well.
