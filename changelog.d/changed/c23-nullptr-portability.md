- Add a compiler-selected `VMAF_NULLPTR` token plus native and forced-fallback
  compile tests, so C translation units can satisfy `modernize-use-nullptr`
  without risking the required MSVC C lane or suppressing diagnostics.
