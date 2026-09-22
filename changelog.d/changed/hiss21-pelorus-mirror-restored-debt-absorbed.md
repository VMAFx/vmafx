- **refactor(core):** Restore `core/src/interop/pelorus_*.c` to a verbatim copy
  of the pinned Pelorus source and absorb the debt that came back with it. The
  HISS-21 burn-down had split four functions inside the vendored mirror, which
  ADR-1113 forbids; reconciling with the v0.2.2 re-vendor reinstated nine
  recorded HISS findings. Rather than raise the ratchet, twelve real findings
  were cleared in the fork's own code: the cascading allocation-unwind `goto`
  labels in `vmaf_model_collection_append`, the `goto fail` unwinds in
  `float_adm.c`'s `init`, the forward `goto write_score` in
  `integer_motion.c`'s `extract`, and four oversized functions split at
  statement boundaries (`float_adm.c` `extract`, `integer_motion.c` `init` and
  `flush`). The debt baseline therefore fell 279 → 276 with no exception flag.
  Scores are bit-identical: all three Netflix golden pairs produce byte-equal
  JSON at `--precision max` before and after. The four splits still want
  landing upstream in `VMAFx/pelorus`. (ADR-1113, ADR-0141)
