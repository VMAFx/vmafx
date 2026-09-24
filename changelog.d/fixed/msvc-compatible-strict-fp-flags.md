- Use compiler-native strict floating-point options for MSVC, Intel `icx-cl`,
  clang-cl, AArch64 carve-outs, and the Windows nvcc host compiler instead of
  passing Unix-only flags that those drivers warned about and ignored.
