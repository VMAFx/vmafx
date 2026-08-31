- Build the `-oneapi2025` production container from Intel's oneAPI Base Toolkit
  image tagged 2025.3.2 while retaining Intel's latest published 2025.3.1 runtime image;
  both inputs remain digest-pinned and the final image is runtime-smoked. The
  Ubuntu 24.04 GPU builders now install a checksum-pinned Meson 1.12.0 wheel
  because the distribution's Meson 1.3.2 is below the source tree's floor.
  Production CPU and GPU images also place bundled model files directly under
  the documented `/usr/local/share/vmafx/model` directory instead of an
  unintended nested `model/` subdirectory.
