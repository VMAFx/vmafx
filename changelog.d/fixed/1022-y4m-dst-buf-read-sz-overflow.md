### fix(y4m): cast `dst_buf_read_sz` operands to `size_t` to prevent signed overflow and heap underallocation (ADR-1022)

Five `dst_buf_read_sz` arithmetic expressions in `core/tools/y4m_input.c` used bare
`int * int` multiplication for the 420/420jpeg/420mpeg2, 420p10, 420p12, 422p10, and
422p12 chroma branches. A crafted Y4M header with `pic_w` / `pic_h` near `INT_MAX/2`
triggered signed-integer overflow, underallocating `dst_buf_read_sz` relative to the
malloc'd `dst_buf_sz`; the subsequent `fread` at line 931 could read past the heap
buffer. Added `(size_t)` casts to mirror the pattern already applied to `dst_buf_sz`
and to all other `dst_buf_read_sz` branches in the same function.
