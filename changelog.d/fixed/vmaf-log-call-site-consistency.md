Restore standardized `vmaf_log()` call sites after a silent revert: add missing
trailing `\n` in `luminance_tools.cpp` (2), `speed.c` (2), and `vif.c` (1);
remove the redundant `"Error: "` prefix from all three CUDA initialization
messages in `cuda/common.c` (the log level tag already conveys severity).
