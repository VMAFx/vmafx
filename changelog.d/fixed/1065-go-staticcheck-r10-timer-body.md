- **Go: poll-loop timer leak** (`pkg/storage`) — `waitForHTTP` and `waitForPath`
  called `time.After(interval)` inside a `for`/`select` loop, allocating a new
  `*time.Timer` on each iteration; the timer leaked until it fired when the
  `ctx.Done()` arm won the select.  Replaced with a single `time.NewTicker` per
  call (ADR-1065).
- **Go: missing `MaxBytesReader` on controller `/v1/score`** — the
  `vmafx-controller` score handler read the JSON body without a size cap, allowing
  a caller to push an arbitrarily large body.  Added `MaxBytesReader(1 MiB)` and
  HTTP 413 mapping, matching the guard already present in `vmafx-server` (ADR-1065).
- **Go: missing `ReadTimeout` on both HTTP servers** — `ReadHeaderTimeout` was set
  but `ReadTimeout` (which covers the body-read phase) was absent.  Added
  `ReadTimeout: 30s` to both `vmafx-controller` and `vmafx-server` (ADR-1065).
