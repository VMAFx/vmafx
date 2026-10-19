- Build: libvmaf compiles on macOS again. `feature_collector.h` included C++ standard headers
  (through `model.h`) inside an `extern "C"` block, which libc++ rejects with "templates must
  have C++ linkage"; headers are now included before the C-linkage block.
