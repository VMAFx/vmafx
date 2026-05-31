<!-- markdownlint-disable MD013 -->
# Research 0102: vmaf-tune compare bisect CLI closure

Date: 2026-05-14

## Question

Which remaining `vmaf-tune` scaffold can be closed without new model
training or external corpus collection?

## Findings

- `vmaftune.bisect` already ships the real Phase-B encode+score
  binary search primitive and `make_bisect_predicate(...)`.
- `vmaftune.compare.compare_codecs()` already accepts a per-codec
  predicate seam and ranks successful rows by bitrate.
- The CLI still exposed only the report orchestration. Without a
  custom programmatic predicate, users got failure rows pointing them
  at `make_bisect_predicate(...)`; the documented `--predicate-module`
  escape hatch was not wired either.
- Raw-YUV compare cannot infer geometry, so a real CLI default needs
  explicit `--width` / `--height` plus the scorer shape flags.

## Decision

Wire `vmaf-tune compare` to `make_bisect_predicate(...)` by default
when source geometry is supplied. Keep `--predicate-module
MODULE:CALLABLE` as the explicit custom/test backend hook. Leave the
programmatic `compare_codecs()` default as a clean `ok=False` row
because its bare predicate signature does not carry geometry.

## Alternatives Considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| Keep CLI scaffold and require Python callers to bind the bisect | No CLI surface change | User-facing subcommand remains non-functional by default | Rejected |
| Add a separate `vmaf-tune bisect` command first | Clean single-codec primitive | Does not close the existing `compare` scaffold | Defer |
| Auto-probe geometry for every source | Fewer flags for containers | Raw YUV remains ambiguous; adds ffprobe failure modes before the existing scorer path | Defer |
| Chosen: require geometry flags and bind real bisect | Closes the user-facing scaffold with the existing Phase-B primitive | Raw-YUV users must pass shape explicitly | Accepted |

## Validation

```bash
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m pytest tools/vmaf-tune/tests/test_compare.py -q
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m ruff check tools/vmaf-tune/src/vmaftune/cli.py tools/vmaf-tune/src/vmaftune/compare.py tools/vmaf-tune/tests/test_compare.py
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m black --check tools/vmaf-tune/src/vmaftune/cli.py tools/vmaf-tune/src/vmaftune/compare.py tools/vmaf-tune/tests/test_compare.py
```

## References

- `req`: user request 2026-05-14, "there are a lot of modules that arent even coded?"
- `req`: user request 2026-05-14, "there are enough scaffolds left in tune especially so go on"
- ADR-0326: `vmaf-tune` Phase B target-VMAF bisect.
- `tools/vmaf-tune/AGENTS.md`: compare predicate and Phase-B bisect invariants.
