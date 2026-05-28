## vmafx-tune-go Stage 3 — resolution-aware downscaling, `--workers`, conformal intervals, `bisect` subcommand (ADR-0734)

Extends the Stage-2 `ladder` subcommand with the three items deferred from
Stage 2, and ports a new `bisect` subcommand.

**`ladder` Stage-3 additions:**

- **Resolution-aware downscaling**: the source is scaled to each rendition
  resolution (Lanczos, `scale=W:H:flags=lanczos`) before each encode/score
  iteration. VMAF is now measured at the actual playback resolution rather than
  the native source resolution. QSV hardware encoders merge the scale filter
  with the VA-API hw-upload chain to avoid conflicting `-vf` flags.

- **Concurrent grid sampling** (`--workers N`): up to N encoder+scorer goroutine
  pairs run in parallel, bounded by a `chan struct{}` semaphore of size N.
  Default = `NumCPU/2` clamped to `[1, 8]`. The pre-allocated cloud slice
  eliminates slice-growth races — each goroutine writes to its own index.

**New package `pkg/conformal`**:

- `conformal.Compute(samples, coverage)` returns a split-conformal prediction
  interval `[lo, hi]` at the requested coverage level (default 90 %).
  Algorithm: absolute residuals from the mean as non-conformity scores;
  empirical quantile at `⌈(M+1)*(1−coverage)⌉/M`. Valid under exchangeability
  (Angelopoulos & Bates 2021). CLI wiring into the ladder sampler is deferred
  to Stage 4 (collecting M samples per grid cell multiplies wall time by M).

**New subcommand `bisect`** (`pkg/bitratesearch`):

- Binary search in the **bitrate domain** (VBR mode, `-b:v`): finds the minimum
  bitrate that achieves a target VMAF score for a fixed encoder.
- Flags: `--encoder`, `--target-vmaf`, `--bitrate-min`, `--bitrate-max`,
  `--tolerance`, `--max-iter`, `--scale-width`, `--scale-height`.
- JSON output (`schema_version: 1`) mirrors the Python `vmaf-tune bisect`
  schema; field names are identical.
- `bisect` removed from the stub list in `cmd/vmafx-tune/cmd/root.go`.

See [docs/usage/vmafx-tune-go.md](docs/usage/vmafx-tune-go.md) and
[ADR-0734](docs/adr/0734-vmafx-tune-go-stage3.md).
