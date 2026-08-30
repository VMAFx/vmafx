### simd: make motion_{avx2,avx512,neon}.h self-contained for ptrdiff_t

`motion_avx2.h`, `motion_avx512.h`, and `motion_neon.h` all declare
function parameters of type `ptrdiff_t` but included only `<stdint.h>`,
relying on `<stddef.h>` being pulled in transitively by consumer `.c`
files (via `mem.h`). A standalone include of any of these headers
fails on Apple Clang and Ubuntu ARM Clang with "unknown type name
'ptrdiff_t'" — the same family of bug fixed in PR #914 for the
`cambi_*.h` headers. Added `#include <stddef.h>` to each header.
