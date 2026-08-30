**gpu_picture_pool.cpp: clear `*pool` to `nullptr` on all failure paths in `vmaf_gpu_picture_pool_init`.**
The C++ translation of the GPU picture pool init function omitted the `*pool = nullptr`
assignment at the `free_p` label (reached when the pic-array `malloc` fails or
`pthread_mutex_init` fails). When the struct-level `malloc` succeeded but a subsequent
step failed, the caller's handle was left pointing to the now-freed pool struct, causing
a use-after-free on the next `vmaf_close()` call. The companion C stub (`gpu_picture_pool.c`)
already carried the null-clear; this aligns the C++ implementation. Caught by
`test_gpu_picture_pool_uaf` (exit status 1 in Ubuntu gcc static + Ubuntu clang CI jobs).
