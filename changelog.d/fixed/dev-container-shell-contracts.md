- **Dev container builds:** make pipeline failure handling explicit per stage,
  export the ccache directory to both libvmaf configure and compile commands,
  and use explicit build paths before cleanup. Golden-test collection failures
  retain their diagnostics; Go artifact counting handles filenames directly and
  the artifact stage returns to the unprivileged build user.
