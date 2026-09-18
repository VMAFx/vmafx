- **`libvmaf.so` no longer exports internal symbols from its C++ sources.** The
  C++ translation units compiled into the library were built without the
  library's common flags, `-fvisibility=hidden` among them, so the shared
  object exported 72 symbols outside its public API — `aligned_malloc`,
  `aligned_free`, `picture_copy`, `mkdirp` and 64 private `vmaf_*` functions
  such as `vmaf_dictionary_copy`. A host application defining any of those
  names would silently replace libvmaf's own implementation. Every C++ target
  now takes the same flags as the C sources, and a new `check_exported_symbols`
  test fails the build if the export table grows past the public API again.
  No public symbol changes; nothing outside the library called the removed ones.
