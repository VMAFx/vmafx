- Four new sub-agent definitions under `.claude/agents/` per audit
  slice G coverage gaps:
  - `hip-reviewer.md` — reviews `core/src/{hip,feature/hip}/`
    code; classifies stub-vs-real status, CUDA-twin parity,
    `enable_hipcc` build-mode awareness.
  - `metal-reviewer.md` — reviews `core/src/{metal,feature/metal}/`
    Obj-C++ + MSL pairs; checks Apple-Family-7 gating, ARC
    correctness, IOSurface zero-copy contracts.
  - `pr-body-checker.md` — local mirror of the
    `scripts/ci/deliverables-check.sh` ADR-0108 gate; catches
    prose-bullet vs `- [x]` checkbox failures in <5 s instead of
    after a 90-minute CI cycle. Surfaces "no X needed" opt-outs as
    soft warnings per the 2026-05-15 no-defer rule.
  - `doc-reviewer.md` — reviews any `docs/` change for
    mkdocs-strict conformance, ADR-0100 per-surface bars, ADR-0028
    body-immutability, ADR-0221 fragment discipline, and
    source-vs-doc accuracy.
  Closes audit slice G §G.3 ("Missing reviewer agents") cleanly.
