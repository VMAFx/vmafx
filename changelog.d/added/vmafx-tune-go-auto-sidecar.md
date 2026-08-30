Ported the `auto` and `sidecar` subcommands from Python `vmaf-tune` to
`vmafx-tune-go`, replacing their loud-fail stubs. `auto` runs the full Phase F
decision tree — ffprobe/HDR source probing, the per-content-type recipe
override, all ten short-circuit predicates, the conformal confidence policy,
per-cell CRF/VMAF/bitrate estimation, and Pareto winner selection — and
optionally realises the winning cell through real FFmpeg encodes plus libvmaf
scores (`--execute`). `sidecar` ports the on-host bias-correction model with
its `status` / `predict` / `record` / `batch-record` surfaces, the online-ridge
Sherman-Morrison fit, and the anonymous cache-dir persistence layout.

Both surfaces are byte-compatible with the Python originals, verified against
fixtures dumped from the in-tree Python modules: whole `auto` plans (including
the bare `NaN` token CPython's `json.dumps` emits for an uncalibrated conformal
interval), every sidecar payload and its persisted `state.json`, the 19-entry
codec-adapter table with per-preset ffmpeg argv, ~1,700 predictor vectors, and
40 sequential ridge updates.

Two supporting packages make that byte-compatibility possible:
`pkg/tune/pyjson` reproduces CPython's `json.dumps(..., sort_keys=True)`
spelling, and `pkg/tune/pymath` supplies correctly-rounded `Exp2` / `Log10`
because Go's `math.Pow` and `math.Log10` land one ULP from the platform libm —
enough to move the last digit of `estimated_bitrate_kbps` and `estimated_vmaf`.

New CLI surfaces are documented in `docs/usage/vmafx-tune-go.md`.
