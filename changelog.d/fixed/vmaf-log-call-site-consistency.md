Standardize `vmaf_log()` call sites: add missing trailing `\n` in
`luminance_tools.c` (2), `speed.c` (2), and `vif.c` (1); remove
redundant `"Error: "` prefix from two `cuda/common.c` messages (the
log level tag already conveys severity).
