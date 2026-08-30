### chore(ci): add concurrency groups + shell-strict to curl|tar steps

Adds top-level `concurrency:` blocks with `cancel-in-progress: true` to three
CI workflows that lacked them, so stale runs are cancelled when a new push or
PR-update lands on the same ref:

- `.github/workflows/go-ci.yml`
- `.github/workflows/rust-ci.yml`
- `.github/workflows/docker-image.yml`

Group key follows the existing `sanitizers.yml` / `build.yml` pattern:
`<workflow-slug>-${{ github.workflow }}-${{ github.ref }}`.

Additionally hardens three `curl | tar` install steps (ONNX Runtime tarball
fetch) with `set -euo pipefail` as the first line of the `run:` block, plus
`curl -fSL --retry 3` so a 5xx blip or a partial body fails fast rather than
silently producing a corrupt extraction:

- `.github/workflows/build.yml` (ONNX Runtime — Linux DNN leg)
- `.github/workflows/tests-and-quality-gates.yml` (ONNX Runtime — Tiny AI job)
- `.github/workflows/libvmaf-build-matrix.yml` (ONNX Runtime — DNN leg)

No user-visible behaviour change; CI hygiene only.
