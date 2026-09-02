- Fixed two master-side CI breaks blocking every open PR:
  - cppcheck `nullPointer` false-positive at `core/src/dict.c:121` —
    removed a redundant `&& val` guard inside `dict_overwrite_existing`
    (`val` is already checked at the public entry-point
    `vmaf_dictionary_set` line 137).
  - `pthread_once_t` use in `core/src/feature/integer_adm.h:45` (added
    by #548 for the TSan `SAN-INTEGER-ADM-DIV-LOOKUP-RACE` fix) didn't
    compile on Windows MSVC + CUDA / oneAPI SYCL; wrapped pthread bits
    in `#ifndef _WIN32` and dropped to direct populate on Windows
    (race is benign — every thread writes identical loop-invariant
    values, and TSan only runs on Linux).
