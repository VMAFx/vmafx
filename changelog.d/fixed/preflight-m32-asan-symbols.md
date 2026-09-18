- **`make preflight` no longer fails on code the 32-bit CI lane never
  builds, and `check_exported_symbols` passes in a GNU-ld sanitizer build.**
  The 32-bit sweep now skips the ISA-specific and GPU source trees, which the
  i686 lane leaves out (`-Denable_asm=false`), and recognises gcc's English
  missing-header message. The exported-symbol check treats the linker-defined
  bounds of ASan and SanitizerCoverage metadata sections as runtime symbols
  rather than a libvmaf API leak.
