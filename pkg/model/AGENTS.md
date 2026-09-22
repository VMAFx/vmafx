# AGENTS.md — pkg/model

Single source of truth for Go-side libvmaf model defaults and selector
formatting.

## Rebase-sensitive invariants

1. `DefaultVersion` mirrors `VMAF_DEFAULT_MODEL_VERSION`; the existing CI
   single-source check owns that equality.
2. Every Go subprocess caller formats `--model` through `CLIArgument` or
   `CLIArgumentOrDefault`. Do not reintroduce local `modelArg` helpers.
3. `CLIArgument` does not invent a default: empty becomes `version=` to retain
   callers whose defaulting happens earlier. Only `CLIArgumentOrDefault`
   substitutes `DefaultVersion`. Any selector containing `=` passes through.

## Test requirements

```bash
go test ./pkg/model/ ./pkg/bisect/ ./pkg/scorecli/ ./pkg/corpus/ \
  ./pkg/fast/ ./pkg/tune/executor/
```
