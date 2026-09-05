- `vmafx-tune predict --use-saliency` now wires saliency moments into
  feature extraction via `pkg/saliency.ComputeMap` rather than returning an
  unimplemented error directing users to the retired Python binary (#1272).
  When inference is unavailable on the host, it logs a warning and degrades
  saliency moments to 0.0, matching the Python behavior and parity contract.
- Removed stale redirects and comments across `cmd/vmafx-tune/main.go`,
  `cmd/vmafx-tune/cmd/root.go`, `cmd/vmafx-tune/cmd/compare.go`, and
  `docs/usage/vmafx-tune-go.md` pointing to the retired Python binary (#1272).
  Removed dead `stubSubcommand` helper and obsolete Stage-1 documentation.
