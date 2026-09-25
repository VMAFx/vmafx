- Provided repository-owned minimal `matrix.h` and `mex.h` lint stubs under
  `compat/python-vmaf/matlab/include/` and added a `matlab_mex` compilation target in
  `core/meson.build`, allowing the 12 vendored MATLAB MEX translation units to be
  exported to `compile_commands.json` and measured in the whole-tree static analysis
  ratchet. Removed the blanket MATLAB exclusion from CI linting, fixed dead stores in
  `ical_std.c`, and recorded the measured 178 warnings in the CPU baseline (ADR-1322).
