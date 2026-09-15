- **Two Semgrep findings in this branch's own new files are suppressed with
  their justification inline**, in the ADR-0278 style the repository already
  uses for its other 56 suppressions. `test_windows_cuda_compiler_discovery.py`
  invokes a fixed Meson path with an argv of literals and tmpdir paths it just
  created, and repoints `PATH` at its own fixture directory — which is the point
  of the test, not tainted input. `scorecard_gate.py` recomputes a **git blob
  object id**, which is SHA-1 by definition: the algorithm is dictated by the
  object format, and the code already passes `usedforsecurity=False`.
