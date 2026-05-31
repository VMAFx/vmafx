- **CWE-78 shell-injection sweep across `compat/python-vmaf/core/`
  (final-area security audit)** — seven subprocess call sites
  (`executor.py` ffmpeg invocation + six MATLAB feature-extractor
  invocations in `matlab_feature_extractor.py`) historically built
  command strings via `' '.join(cmd_list)` and shelled out with
  `subprocess.check_output(..., shell=True)`. Asset paths flowed
  unescaped through that join, so a reference / distorted YUV path
  containing `;`, `|`, `$`, backticks, or other shell metacharacters
  would have been interpreted by `/bin/sh` rather than treated as
  a single filesystem path — a textbook command-injection vector
  (CWE-78). All seven sites now build an argv list and exec the
  child without a shell (`shell=False`). The MATLAB sites grew a
  small helper `_run_matlab(matlab_bin, matlab_script,
  log_file_path=...)` that preserves the previous `>> {log_file_path}`
  redirect semantics by opening the log file in Python (append-mode
  binary `open`) and wiring it through `stdout=` on
  `subprocess.run(...)`, while keeping stderr folded into stdout to
  match the prior diagnostic capture. No Netflix golden assertions
  were touched; subprocess argv-vs-string is transparent to
  numerical outputs.
