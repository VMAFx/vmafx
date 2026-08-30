- **core / cambi**: Fix CAMBI option-parser writing a 4-byte `int`
  through a 2-byte `uint16_t *` for the `window_size` and
  `max_log_contrast` options. The misalignment was flagged by UBSan on
  every `--feature cambi` invocation; the silent struct corruption
  (writing 4 bytes into a 2-byte field clobbering adjacent state) was
  not previously visible. Adds shadow `int` slots in `CambiState` and
  copies them into the existing `uint16_t` runtime fields in `init()`.
  Bit-exact with prior behaviour on every valid option value.
  ([ADR-0869](docs/adr/0869-sanitizer-pass-cleanup.md))
- **core / adm (AVX2 + AVX-512)**: Fix C undefined behaviour in the
  ADM DWT2 filter packing: `(int) << 16` on a negative `filter[k]`
  was UB even though the result was cast to `uint32_t` (precedence:
  cast on result, not on operand). Move the cast inside the shift
  (`((uint32_t)filter[k] << 16)`). Bit-exact with prior wrap-on-overflow
  behaviour on every two's-complement target.
  ([ADR-0869](docs/adr/0869-sanitizer-pass-cleanup.md))
