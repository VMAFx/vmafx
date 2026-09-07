- The Ubuntu 24.04 GPU builders install a checksum-pinned Meson 1.12.0 wheel
  because the distribution's Meson 1.3.2 is below the source tree's floor.
  Production CPU and GPU images also place bundled model files directly under
  the documented `/usr/local/share/vmafx/model` directory instead of an
  unintended nested `model/` subdirectory. (The oneAPI builder/runtime pins this
  fragment originally described were superseded before release by the move to
  oneAPI 2026.1 — see the `-oneapi2026` entry.)
