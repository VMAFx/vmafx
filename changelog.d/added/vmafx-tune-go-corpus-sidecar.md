Added the `corpus` and `sidecar` subcommands to `vmafx-tune-go`, closing the
Go-migration gap for the corpus-state group (ADR-0703 / ADR-0704). `corpus` runs
the Phase A `(preset, crf)` grid sweep — full grid or 2-pass coarse-to-fine —
with HDR detection, sample-clip mode, 2-pass encoding, encoder-internal stats
capture, resolution-aware model selection and strict scoring-backend selection,
and writes the v3 JSONL contract. `sidecar` exposes `status` / `predict` /
`record` / `batch-record` against the ADR-0394 on-host bias-correction model,
sharing its on-disk state format with the Python implementation.

Five new Go packages back them: `pkg/codecadapter` (the ADR-0237 registry for all
19 codecs), `pkg/corpus` (encode / score / HDR / shots / stats / backend / JSONL
/ coarse-to-fine), `pkg/predictor` (the analytical VMAF curve), `pkg/sidecar`
(online ridge with Sherman-Morrison updates), and `pkg/pyjson` (a
CPython-compatible JSON encoder — the corpus JSONL carries bare `NaN` tokens and
`repr()`-style floats that `encoding/json` cannot produce).

Verified against the Python implementation on real encodes: a 2-cell grid sweep,
a 15-cell coarse-to-fine sweep and a `--two-pass --sample-clip-seconds
--force-hdr-pq` run all produce byte-identical JSONL (modulo the per-run
`run_id` / `timestamp` / wall-clock columns), and sidecar state written by either
binary loads in the other with bit-identical predictions.

`vmafx-tune-go sidecar --model <predictor.onnx>` is accepted and resolves the
model through `pkg/ai`, which shells out to a `vmafx-ort-runner` subprocess
rather than linking ONNX Runtime into the Go binary. That runner is not built by
this repository yet, so inference reports `ai.ErrORTRunnerNotFound` and the
predictor falls back to the analytical curve — now with a warning, instead of
silently. An earlier draft of this note said the Go binary *refuses* the flag;
that described the group-3 sidecar, but the port integration wired the group-5
implementation, which accepts it. Documented in `docs/usage/vmafx-tune-go.md`.
