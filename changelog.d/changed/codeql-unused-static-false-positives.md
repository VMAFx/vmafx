- Code scanning: `core/src/cpu.cpp` writes `constinit` after the type of its
  two CPU-flag atomics and the deprecated `VMAF_SYCL_NO_GRAPH` warning in
  `core/src/sycl/dispatch_strategy.cpp` uses an atomic bool, so CodeQL's
  `cpp/unused-static-variable` no longer reports the three variables it misread
  (alerts 1551 to 1553). Behaviour and the constant-initialisation check are
  unchanged.
