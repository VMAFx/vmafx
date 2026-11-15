---
paths:
  - pkg/bisect/*.go
  - pkg/encoder/*.go
  - pkg/storage/*.go
  - cmd/vmafx-controller/queue/*.go
invariant: Combine primary + cleanup errors via errors.Join; ignore os.ErrNotExist; slog key = error.
---
<!-- markdownlint-disable MD013 MD029 -->
# Multi-step cleanup error handling (`errors.Join`, ADR-0935)

## Rebase-sensitive invariants

7. **`errors.Join` for multi-step cleanup** (`pkg/bisect`, `pkg/encoder`,
   `pkg/storage`, `cmd/vmafx-controller/queue` — and any new sibling package
   growing similar pipeline): when primary error and cleanup error
   can both arise, return `errors.Join(primary, cleanup)` rather than
   silently dropping cleanup error via `_ = X()` or
   `X() //nolint:errcheck`. Guard cleanup `os.Remove` calls with
   `errors.Is(rmErr, os.ErrNotExist)` so not-yet-created file is not
   flagged as cleanup failure. `slog` error-attribute key =
   `"error"` everywhere (`"err"` retired). See ADR-0935.
