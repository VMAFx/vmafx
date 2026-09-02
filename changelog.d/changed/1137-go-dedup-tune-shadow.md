- **refactor(go):** Consolidated the duplicated Go packages the seven-way
  `vmafx-tune` port integration left behind (ADR-1137). One CPython-compatible
  JSON encoder (`pkg/pyjson`, replacing `internal/pyjson`,
  `internal/pyjsonstrict` and the `pkg/tune/pyjson` implementation), one
  ffmpeg argv builder and version parser (`pkg/ffencode`, with `pkg/corpus`,
  `pkg/encodeprofile` and `pkg/tune/executor` now thin aliases), one codec
  registry (`pkg/codecadapter`), one predictor (`pkg/predictor`, which also
  takes over the ORT-session wiring from `cmd/vmafx-tune`), one HDR port
  (`pkg/hdr`) and the libm-parity layer at `pkg/pymath`. The Python-derived
  parity fixtures moved with the survivors and `pkg/predictor` pins nine
  CPython-computed curve values as raw bits. `pkg/tune/{codec,predictor,pyjson}`
  remain as transitional one-file aliases because the sidecar files that import
  them are owned by the in-flight #1187; they are deleted once it lands.
  User-visible deltas: `vmafx-tune auto --execute` no longer emits the AMF
  adapters' inert duplicate `-quality/-rc/-qp_i/-qp_p` tail, matching the other
  encode drivers, and `auto` / `sidecar --model` log one warning when
  `vmafx-ort-runner` is absent (falling back to the analytical curve as before),
  as `predict --model` already did. Net −2,116 non-test lines of Go.
