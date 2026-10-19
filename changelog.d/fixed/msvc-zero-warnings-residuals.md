- The last MSVC warnings of the first Windows run after the zero-warning series
  are fixed: the CUDA / HIP decouple helpers and the x86 motion round constant
  shift in 64 bits (C4334; the operand never exceeds 30 bits), and three
  conversions in `get_noise_constant()`, the scaled frame size passed to
  `vif_scale_frame_s()` and the second `--feature` option copy are written out.
