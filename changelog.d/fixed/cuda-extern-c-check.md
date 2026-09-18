- **The CUDA `extern "C"` kernel check (`scripts/dev/check-cuda-extern-c.sh`)
  checks kernels again.** It matched no `cuModuleGetFunction` call in the tree
  and then aborted on an empty list, so it never verified anything. It now
  reads every call, confirms that each kernel it can locate sits inside an
  `extern "C"` block, and names the macro-generated kernels it cannot locate.
