- Replaced the Windows MSVC + CUDA CI leg's CUDA setup wrapper with a direct
  NVIDIA CUDA 13.2.0 network-installer step so the required build-only gate can
  reach Meson/Ninja again on current `windows-2025` runners.
