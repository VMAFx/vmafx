- **ci(macos):** Add `mxschmitt/action-tmate` SSH debug step to all macOS
  matrix legs in `libvmaf Build Matrix`. The step fires only on
  `workflow_dispatch`-triggered runs where a preceding step has failed,
  opening a 30-minute tmate session so the operator can run `lldb` on the
  crashing binary directly on the GitHub-hosted runner. Zero cost on
  regular PR pushes. (ADR-0626)
