- **`vmafx-tune-go fast` — Phase A.5 fast path ported to Go**: the
  proxy + TPE + GPU-verify recommend subcommand from
  `tools/vmaf-tune/src/vmaftune/fast.py` now exists in the Go binary with the
  same flag names, defaults, JSON schema (byte-compatible down to CPython's
  float `repr` formatting) and `0 / 2 / 3` exit-code contract. `--smoke` runs
  end to end on any host. Production mode runs backend selection, the probe
  encodes, the canonical-6 extraction and the mandatory verify pass, then stops
  at proxy inference — see the known limitation below. New packages:
  `pkg/fast` (search, pipeline, proxy seam, JSON encoder), `pkg/scorebackend`
  (libvmaf backend detection + strict selection, ported from
  `score_backend.py`), and `pkg/conformal` (split-conformal and CV+ /
  jackknife+ prediction intervals, ported from `conformal.py`, with a JSON
  sidecar byte-compatible with the Python writer). The TPE search is backed by
  `github.com/c-bata/goptuna` v0.9.0, a Go implementation of Optuna's
  Tree-structured Parzen Estimator; it and `gonum.org/v1/gonum` are the only
  new module dependencies. (ADR-0276, ADR-0304, ADR-0705)

- **Known limitation — `fast` production mode is blocked on ONNX named
  inputs**: `model/tiny/fr_regressor_v2.onnx` declares two named input ports
  (`features` `[N, 6]` and `codec` `[N, 14]`), while the Go inference seam
  (`pkg/ai.Registry.Infer` → `vmafx-ort-runner`) accepts a single flat input
  vector. Flattening the ports would feed the graph's 6-D `features` port only
  and leave `codec` empty — the exact mistake `vmaftune/proxy.py` documents —
  so `pkg/fast` fails with `ErrProxyPortsUnsupported` and a diagnostic naming
  both ports rather than returning a silently-wrong score. Unblocking needs a
  named-input runner protocol, a CGO ONNX Runtime binding
  (`Registry.InferDirect`, a Stage-2 stub today), or a single-port re-export of
  the model. Use `vmaf-tune fast` for production runs until then.

- **Known limitation — TPE runs are not bit-reproducible in Go**: Optuna's
  `TPESampler(seed=0)` guarantees a reproducible search; goptuna v0.9.0 honours
  its seed only partially, because `goptuna/internal/random.ArgMaxMultinomial`
  draws from the process-global `math/rand` source (which Go seeds randomly at
  startup and which `rand.Seed` can no longer override). Repeat runs may return
  a neighbouring CRF when two candidates score within about a VMAF point of
  each other. Measured on the ADR-0276 smoke curve: within ±1 of the
  brute-force optimum in ~99–100 % of runs at the shipped budgets, and exactly
  optimal in 150 of 150 runs at 150 trials. Closing the gap needs an upstream
  goptuna change threading the sampler RNG into `internal/random`.

- **`pkg/encoder` input-side ffmpeg options**: `EncodeParams.InputArgs` emits
  options before `-i`, so raw-YUV sources (`-f rawvideo -pix_fmt -s -r`) and
  sample clips (input-side `-ss` / `-t`, for fast-seek rather than
  decode-and-discard) are expressible. `EncodeParams.OutputPath` pins a
  deterministic encode destination and `EncodeResult.OutputSizeBytes` reports
  the encode size for size-over-duration bitrate maths. All three are additive;
  `compare` and `ladder` behaviour is unchanged.
