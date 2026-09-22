- Remove twelve high-signal Cppcheck findings and all pre-existing HISS debt
  from the touched non-CUDA CLI, benchmark, SpEED, tiny-AI, Xiph PSNR-HVS and
  native-test files. CLI/benchmark cleanup is now structured, and benchmark
  frame/flush failures propagate instead of being overwritten by success.
