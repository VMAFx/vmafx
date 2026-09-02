# Research: Python type-annotations audit (2026-05-30)

- **Status**: Active
- **Workstream**: type-annotations cleanup (no associated ADR — chore-class PR)
- **Last updated**: 2026-05-30
- **Author**: scheduled-agent (chore/python-type-annotations-audit)

## Why

The fork has ~150 KLOC of Python across four trees (`ai/src`,
`mcp-server/vmaf-mcp/src`, `tools/vmaf-tune/src`, plus the
upstream-mirrored `compat/python-vmaf`). None of the fork-local trees
had ever been audited under `mypy --strict`; the project-root
`pyproject.toml` already declares `strict = true` but only when invoked
via `mypy` against explicit packages, never as a CI gate. This created
two classes of risk:

1. **Latent real bugs** masked by missing or loose annotations (the
   `dict` without parameters, the `object` fallback when the real type
   is known, the `# type: ignore` that drifted past its expiry).
2. **Maintenance friction** — every new contributor has to re-derive
   the implicit shape of a parameter from call-site reading.

## How (the audit method)

For each fork-local package:

```bash
# ai/src — namespace packages under src-layout
MYPYPATH=ai/src mypy --strict --explicit-package-bases \
    -p vmaf_train -p corpus -p aiutils

# mcp-server
MYPYPATH=mcp-server/vmaf-mcp/src mypy --strict --explicit-package-bases \
    -p vmaf_mcp

# vmaf-tune
MYPYPATH=tools/vmaf-tune/src mypy --strict --explicit-package-bases \
    -p vmaftune
```

`compat/python-vmaf/` is upstream-mirrored and already covered by the
project-root `[[tool.mypy.overrides]]` block `module = "vmaf.*"` →
`ignore_errors = true`. Out of scope for this audit (the rule predates
the fork — see root `pyproject.toml`).

## Findings (per package, baseline → after)

| Package                            | Errors baseline  | Errors after  | Δ     |
| ---------------------------------- | ---------------: | ------------: | ----: |
| `ai/src/vmaf_train,corpus,aiutils` | 7                | 0             | -7    |
| `mcp-server/vmaf-mcp/src/vmaf_mcp` | 16               | 0             | -16   |
| `tools/vmaf-tune/src/vmaftune`     | 261              | 196           | -65   |

The `vmaf-tune` residue is dominated by three families that need
follow-up PRs scoped per-family (this PR does not bundle them to keep
the diff reviewable):

- **CodecAdapter Protocol mismatch** (~17 `[dict-item]` errors in
  `codec_adapters/__init__.py`) — every concrete adapter fails the
  `Protocol` membership check; root cause is likely a method-signature
  drift between the Protocol and at least one concrete adapter.
  Requires a per-adapter signature audit, not a localized fix.
- **`cli.py` mega-file** (67 errors, most `[no-untyped-def]`) — the
  CLI entry-point has ~30 internal helper functions that lack
  annotations. Better tackled as its own follow-up since cli.py is
  4500+ LOC and the helpers are tightly coupled.
- **`predictor_train.py`** (25 errors) — PyTorch / numpy interop that
  needs careful per-call review.

## Bugs surfaced (and fixed)

1. **Duplicate `_run_benchmark` definition**
   (`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`). Two `async def
   _run_benchmark(...)` definitions co-existed in the same module —
   one at line 720 (older, no `progress_token` parameter), one at
   line 1295 (newer, with progress notifications per ADR-0608).
   Python silently rebinds the symbol to the later definition at
   import time, so the older copy was *unreachable dead code* yet
   future edits to it would be invisible at runtime. The fix
   replaces the dead copy with a `NOTE` comment pointing at the
   live implementation.

2. **`seen: set[str]` actually holding tuples**
   (`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`, `list_extractors`).
   Declared `set[str]` but populated with `(sym, name)` tuples; the
   containment check `if key in seen` worked at runtime because Python
   doesn't check element types, but every mypy run would have caught
   this before commit. Fixed to `set[tuple[str, str]]`.

3. **Loop-variable shadowing in `report.py`**
   (`tools/vmaf-tune/src/vmaftune/report.py`, ABR-ladder block).
   `for r in data.ladder_rungs` reused the name `r` from a previous
   `for r in data.codec_rows` loop further up the same function;
   mypy still tracked `r` as `CodecRow` and flagged every
   `LadderRung`-specific attribute access (`r.width`, `r.height`,
   `r.crf`, `r.vmaf`). Renamed to `rung`.

## Bugs surfaced but NOT fixed in this PR

- **CodecAdapter Protocol mismatch** (above) — needs a dedicated
  follow-up PR.
- **`cli.py:4569,4583,4606`** — three `Argument 1 to "int" has
  incompatible type "Any | int | None"` errors. Each call site
  takes a CLI-parsed value that *should* have been validated
  upstream; the fix is per-call-site and benefits from a single
  reviewer pass over `cli.py` as a whole.
- **HTTP transport pre-existing failure** in
  `mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py::_handle_metrics`
  — uses an aiohttp API surface that newer aiohttp rejects
  (`charset must not be in content_type argument`). Unrelated to
  type annotations; flagged for a follow-up.

## Reproducer

```bash
# Re-run the audit from a clean worktree:
MYPYPATH=ai/src mypy --strict --explicit-package-bases \
    -p vmaf_train -p corpus -p aiutils
MYPYPATH=mcp-server/vmaf-mcp/src mypy --strict --explicit-package-bases \
    -p vmaf_mcp
MYPYPATH=tools/vmaf-tune/src mypy --strict --explicit-package-bases \
    -p vmaftune
```

## Follow-ups

- Per-adapter `CodecAdapter` Protocol audit (separate PR).
- `tools/vmaf-tune/src/vmaftune/cli.py` annotation pass (separate PR).
- `tools/vmaf-tune/src/vmaftune/predictor_train.py` numpy/torch
  interop typing (separate PR).
- HTTP transport aiohttp compatibility fix (separate PR, unrelated
  to the type audit).
- Wire `mypy --strict` into CI for the three fork-local trees so
  fresh regressions are caught at PR time.
