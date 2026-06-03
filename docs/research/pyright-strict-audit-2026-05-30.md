# Pyright strict audit — fork-local Python packages (2026-05-30)

**Scope.** Run `pyright` in strict mode against the three fork-local
Python trees that PR #366 already strict-checks with `mypy`:

- `ai/src/{vmaf_train,corpus,aiutils}`
- `mcp-server/vmaf-mcp/src/vmaf_mcp`
- `tools/vmaf-tune/src/vmaftune`

**Why pyright on top of mypy.** Pyright catches a different class of
issues than mypy: cross-procedural `Optional` narrowing through
`raise`/early-return, `reportUnnecessaryComparison` /
`reportUnnecessaryIsInstance` dead-code checks, missing Protocol
fields, ORT-style union-return narrowing, and undefined forward-refs
masked by `# noqa: F821`. None of those surfaced on the mypy pass.

**Constraint.** Skip every file PR #366 modifies, to keep the merge
trivial. The skip-list (19 files) is the union of the PR's diff.

## Method

```bash
pip install pyright==1.1.409
cat > pyrightconfig.audit.json <<'JSON'
{
  "include": ["ai/src", "mcp-server/vmaf-mcp/src", "tools/vmaf-tune/src"],
  "strict":  ["ai/src", "mcp-server/vmaf-mcp/src", "tools/vmaf-tune/src"],
  "pythonVersion": "3.12",
  "pythonPlatform": "Linux",
  "reportMissingImports":  "warning",
  "reportMissingTypeStubs":"warning"
}
JSON
pyright -p pyrightconfig.audit.json --outputjson ai/src/ | jq '.summary'
pyright -p pyrightconfig.audit.json --outputjson mcp-server/vmaf-mcp/src/ | jq '.summary'
pyright -p pyrightconfig.audit.json --outputjson tools/vmaf-tune/src/    | jq '.summary'
```

The config file is intentionally untracked (`pyrightconfig.audit.json`)
because shipping it would surface ~1,600 stub-cascade errors as a CI
gate before the long-tail cleanup is done. Re-run locally for
follow-ups.

## Baseline (master @ 387839e)

| Package | errors | warnings |
| ------------ | -------: | ---------: |
| `ai/src` | 370 | 0 |
| `mcp/src` | 61 | 0 |
| `tune/src` | 1,257 | 0 |
| **Total** | **1,688** | 0 |

## Triage — by rule

The bulk of strict errors are `reportUnknown*Type` cascades from
third-party packages without stubs (torch, scipy, optuna, onnxruntime,
pyarrow). These are noise: the fix is upstream stubs, not fork-code
defects. Filtered counts of high-signal rules across all three trees:

| Rule | Count | Class |
| ------ | ------: | ------- |
| `reportArgumentType` | 94 | type / overload mismatches |
| `reportOptionalMemberAccess` | 9 | Optional access pyright sees, mypy missed |
| `reportAttributeAccessIssue` | 15 | attr on union / stub-incomplete class |
| `reportUnnecessaryComparison` | 8 | dead `is None` / `is not None` branches |
| `reportUnnecessaryIsInstance` | 3 | dead isinstance check |
| `reportRedeclaration` | 2 | duplicate def in same scope |
| `reportReturnType` | 2 | wrong return annotation |
| `reportAssignmentType` | 3 | type narrows wider than declared |
| `reportUndefinedVariable` | 4 | forward-ref string not importable |
| `reportPossiblyUnboundVariable` | 1 | branch-bound name used elsewhere |
| `reportIncompatibleMethodOverride` | 1 | override signature drift |
| `reportUnsupportedDunderAll` | 1 | `__all__` entry missing from module |
| `reportCallIssue` | 4 | overload non-match |

## Fix scope (this PR)

The audit takes the 12 highest-impact sites that fit the
"~15 LOC, no public-API reshape" envelope and applies them in one PR.
Anything that would cascade through the `LadderPoint` union, the
`CodecAdapter` Protocol variance, or the ORT-result narrowing across
the package is deferred.

| # | File | Line | Bug | Fix |
| --: | ------ | -----: | ----- | ----- |
| 1 | `ai/src/vmaf_train/confidence.py` | 24 | `Tensor` used as forward-ref string but never imported (4 occurrences hidden by `# noqa: F821`) | `TYPE_CHECKING` import from torch |
| 2 | `ai/src/vmaf_train/cross_backend.py` | 120,124 | ORT `session.run()` returns union; downstream `.astype` only valid on `ndarray` | `assert isinstance(..., ndarray)` + typed local |
| 3 | `ai/src/vmaf_train/eval.py` | 35,36 | scipy `pearsonr`/`spearmanr` return `*RResult` but bundled pyright stubs lack `.statistic` | `cast(Any, ...)` |
| 4 | `ai/src/aiutils/subprocess_utils.py` | 20,21,45 | `**kwargs: object` poisons overload resolution; missing generic on `CompletedProcess` | `Any` + `cast` + `CompletedProcess[Any]` |
| 5 | `tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py` | 66 | `presets` declared on every concrete adapter but missing from `CodecAdapter` Protocol; `ladder._default_sampler_preset` fails to typecheck | add `presets: tuple[str, ...]` to Protocol |
| 6 | `tools/vmaf-tune/src/vmaftune/ladder.py` | 385 | `_ladder_point_from_row` returns either `LadderPoint` or `UncertaintyLadderPoint`; the two are not in a subclass relation per the class docstring | document the controlled widening + `cast`; preserve runtime contract (tests assert `isinstance(p, UncertaintyLadderPoint)`) |
| 7 | `tools/vmaf-tune/src/vmaftune/bisect.py` | 652 | `workdir_path is not None` always True (bound on both branches above) | drop guard, add comment |
| 8 | `tools/vmaf-tune/src/vmaftune/bisect.py` | 1270 | `runtime_target_vmaf is None` always False (typed `float`) | drop the `is None` half, keep the NaN branch |
| 9 | `tools/vmaf-tune/src/vmaftune/executor.py` | 656 | `enc is not None` always True (typed non-Optional) | drop guard |
| 10 | `tools/vmaf-tune/src/vmaftune/fast.py` | 307 | `optuna` is None when optional extra missing; `_run_tpe` accesses `optuna.logging` directly. Cross-procedural narrowing through `_require_optuna()` invisible to pyright | `assert optuna is not None, "call _require_optuna() first"` |
| 11 | `tools/vmaf-tune/src/vmaftune/cli.py` | 2869 | `warn_stream: object \| None` — `.write(...)` not typecheckable | `TextIO \| None` + `TextIO` import |
| 12 | `tools/vmaf-tune/src/vmaftune/cli.py` | 3478 | `comparison_report` narrowing through `raise` not seen by pyright | `assert comparison_report is not None` after the XOR guard |

## After-PR deltas

| Package | Baseline | After | Δ |
| ------------ | ---------: | ------: | -------: |
| `ai/src` | 370 | 306 | -64 |
| `mcp/src` | 61 | 61 | 0 |
| `tune/src` | 1,257 | 1,236 | -21 |
| **Total** | **1,688** | **1,603** | **-85** |

The mcp delta is zero because every mcp pyright finding lands in
`server.py`, which PR #366 owns. Re-running the audit after that PR
merges should drop the remaining mcp count.

The ai-tree delta (-64) is larger than the 12 fix sites alone would
predict; the `cross_backend.py` and `confidence.py` narrowings
cascade and resolve dozens of transitive `reportUnknown*Type` errors.

## Residue (follow-ups)

- **CodecAdapter Protocol variance** (~17 `reportAssignmentType` in
  `codec_adapters/__init__.py`) — the Protocol declares fields as
  writable; concrete adapters use `frozen=True` dataclasses. Fix is
  a per-field `Final` / read-only audit, scope is its own PR.
- **ORT-result generic narrowing** across the package (`session.run`
  returns a union; we own ~6 call sites that consume it as
  ndarray).  Fix is a typed helper `_run_dense(sess, feed) ->
  np.ndarray` that asserts once.
- **scipy.stats stubs** — `PearsonRResult.statistic` etc. are
  documented public API but the bundled pyright stubs don't expose
  them.  Either vendor a `.pyi` overlay or wait for upstream.
- **Duplicate-def in branch** (`cli.py::_probe`, ~2 instances) — the
  two `def`s live in mutually exclusive `if/else` arms.  Runtime is
  correct; pyright lacks the branch-exclusivity inference.
  Cosmetic; one-line refactor to a ternary closure resolves it.
- **`onnxruntime` CalibrationDataReader override** —
  `quantize.py::get_next` returns `dict | None` (documented API)
  but the base stubs declare a non-Optional return. Upstream stub
  issue; suppress with `# type: ignore[override]` is the only
  in-tree fix that doesn't break runtime.

## Reproducer

```bash
git checkout chore/pyright-strict-audit
pip install pyright==1.1.409
pyright -p pyrightconfig.audit.json --outputjson ai/src/ | jq '.summary'
pyright -p pyrightconfig.audit.json --outputjson mcp-server/vmaf-mcp/src/ | jq '.summary'
pyright -p pyrightconfig.audit.json --outputjson tools/vmaf-tune/src/    | jq '.summary'
```

(The audit config is gitignored; the path above is the one this
audit used. Drop the `-p` flag for a one-off run with pyright's
defaults — strict mode requires the config since the CLI has no
`--strict` flag.)
