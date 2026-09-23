- **Semgrep warning alerts 946–949 audited and resolved (Research-2078)** — upgraded
  memoization keys to SHA-256 and audited group IPC socket permissions. Fixed:
  (1) `compat/python-vmaf/tools/decorator.py` (`@persist`, `@persist_to_file`, `@persist_to_dir`)
  upgraded from SHA-1 to `hashlib.sha256(..., usedforsecurity=False)` with backward-compatible
  read-through migration: legacy SHA-1 entries and files are read and promoted atomically to
  SHA-256 without recomputation, new entries strictly use SHA-256, and Semgrep alerts 947, 948,
  and 949 are resolved with reviewed compatibility-only exception comments;
  (2) `ai/sidecar/online_trainer.py` audited for Semgrep alert 946 (mode `0o660`): verified
  that `0o660` is an intentional, least-privilege Unix domain socket permission
  required for same-group Go peer IPC in Kubernetes pod emptyDir mounts (connecting
  requires `stat.S_IWGRP`, whereas Semgrep's suggested `0o644` breaks client connection
  and leaks world read); hardened server lifecycle with deterministic socket cleanup and
  optional stop event support; added red-capable regression suites in `test_decorator_extended.py`
  and `test_socket_permissions.py` (mode 0o660, rw- user/group, zero world access, EUID/EGID ownership).
