- CI: the Python lock checker no longer refuses a step before `actions/checkout` that runs
  `jq -c`, `bash -c` or another tool's own `-c` / `-r` / `-e` flag; it reads those flags as a
  requirements file or an editable target only in a pip-like command
  (`T-LOCK-CHECKER-DASH-C-FALSE-POSITIVE-2026-10-18`).
