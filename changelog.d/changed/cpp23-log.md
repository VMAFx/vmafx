Convert `core/src/log.c` to `log.cpp` (C++ Wave 1, ADR-0722): level tables
are now `constexpr std::array<const char *, 4>` with compile-time-fixed size;
bounds clamping uses a typed `clamp_val<T>` helper; `log.h` gains
`extern "C"` guards. No change to public ABI or log output.
