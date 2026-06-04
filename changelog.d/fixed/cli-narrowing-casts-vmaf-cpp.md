- **cli:** Fix `-Wc++11-narrowing` errors in `VmafPictureConfiguration` initializer list
  (`core/tools/vmaf.cpp`). Three `int` to `unsigned` implicit conversions in the
  `pic_params` designated initializer (`.w`, `.h`, `.bpc`) are replaced with explicit
  `static_cast<unsigned>(...)`. Clang with `-std=c++11` strict mode treated these as
  hard errors; GCC silently accepted them. No functional change — values and semantics
  are identical.
