- libvmaf builds against libc++ 23 again. The vendored libsvm
  (`core/src/svm.cpp`) defined its own global `swap` template, and libc++ 23's
  `std::vector` internals now call `swap` unqualified, so both it and
  `std::swap` matched and the file failed with "call to 'swap' is ambiguous".
  libsvm now uses `std::swap`; scores are unchanged.
