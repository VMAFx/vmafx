---
paths:
  - scripts/setup/*
invariant: Per-OS setup dispatcher stays idempotent; never sudo-escalates without user input.
---
<!-- markdownlint-disable MD013 MD038 -->
# Per-OS setup dispatcher (`scripts/setup/detect.sh`)

## Rebase-sensitive invariants

### `setup/detect.sh` is the per-OS dispatcher

Dispatcher reads `/etc/os-release` on Linux, `$OSTYPE` on
macOS, picks `setup/<distro>.sh` or `setup/macos.sh`. Adding
new distro = one new file (`setup/foo.sh`) plus one branch in
`detect.sh`. Dispatcher idempotent, never sudo-escalates
without user input — keep both invariants on rebase.
