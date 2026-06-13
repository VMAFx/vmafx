- **CUDA build fixed against gcc-16 host libstdc++.** Adds `--std c++20`
  to the nvcc invocation in `core/src/meson.build`. nvcc's default
  C++17 host parser chokes on the C++20 features (`char8_t`,
  `constexpr` semantics in `bits/utility.h`) that gcc 16's bundled
  libstdc++ uses; symptom on dev machines with `pacman` `gcc 16.1.1` +
  `cuda 13.2`: `error: identifier "char8_t" is undefined`,
  `error: missing initializer for constexpr variable` across every
  `.cu`. CUDA 13.x supports `c++20` natively; the flag is a no-op on CI
  runners (Ubuntu 24.04 + gcc 13). Tested locally:
  `meson setup build-cuda -Denable_cuda=true && ninja -C build-cuda`
  builds clean → `tools/vmaf` runs with `--gpumask=0`, `--no_sycl`,
  `--no_vulkan` against Netflix golden refs (vmaf_v0.6.1, BigBuckBunny
  1920x1080 25fps).
