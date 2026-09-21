- **`OTEL_TRACES_SAMPLER_ARG=NaN` was accepted as a trace sample ratio.**
  `pkg/observability.InitOTel` resolves the environment override only when it
  parses *and* lands inside `[0, 1]` (ADR-0927). Lifting that resolution into
  its own function rewrote the accept predicate
  `err == nil && parsed >= 0 && parsed <= 1` as the reject predicate
  `err != nil || parsed < 0 || parsed > 1`. Those read as De Morgan duals but
  are not equivalent over floats: every comparison against NaN is false, so the
  accept form rejected NaN while the reject form found nothing to reject and
  returned it. `strconv.ParseFloat` parses `"NaN"` — and `"nan"`, `"NAN"` — to
  NaN with a **nil** error, so the value reached
  `sdktrace.TraceIDRatioBased(NaN)`, whose `uint64(NaN * (1 << 63))` bound is
  undefined: a deployment could silently sample no traces at all. The accept
  predicate is restored and pinned by a table test covering NaN, both
  infinities, both bounds and the out-of-range and unparsable cases.
