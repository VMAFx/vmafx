<!-- markdownlint-disable MD060 -->
# ADR-0936: Final `os.path` → `pathlib.Path` sweep + ruff PTH guard

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude
- **Tags**: python, lint, modernization, build

## Context

The VMAFX modernization roadmap (`project_vmafx_phase4_language_modernization`)
calls for fork-owned Python to consistently use `pathlib.Path` instead of the
legacy `os.path` string-juggling APIs. Prior sweeps left two `os.path`
usages in the repo, both in the `tools/vmaf-*` console shims:

```text
tools/vmaf-roi-score/vmaf-roi-score:16 _HERE = os.path.dirname(os.path.abspath(__file__))
tools/vmaf-roi-score/vmaf-roi-score:17 sys.path.insert(0, os.path.join(_HERE, "src"))
tools/vmaf-tune/vmaf-tune:15           _HERE = os.path.dirname(os.path.abspath(__file__))
tools/vmaf-tune/vmaf-tune:16           sys.path.insert(0, os.path.join(_HERE, "src"))
```

Without a lint guard the pattern silently regressed in copy-paste-derived
files. Enabling ruff's `PTH` ruleset (flake8-use-pathlib) catches the entire
family — `os.path.*`, `os.replace`, `os.symlink`, `os.path.getsize`,
`os.path.splitext`, builtin `open()`, `glob.glob` — at lint time so the
modernization stays sticky.

Enabling `PTH` surfaced 10 additional fork-owned violations across `ai/src/`,
`mcp-server/`, `scripts/ci/`, and `tools/vmaf-tune/` — all are trivial
mechanical conversions (1–2 lines each) and were fixed in the same PR rather
than left as `noqa` debt. Upstream Netflix code under `python/**`,
`compat/python-vmaf/**`, and `testdata/**` retains the `PTH` exception (same
policy as other style-family suppressions per ADR-0100): Netflix style is
not the fork's canon, and forcing pathlib through their tree would create
needless rebase noise.

## Decision

We will:

1. Replace the final two `os.path.{dirname,abspath,join}` usages in the
   `tools/vmaf-*` console shims with `Path(__file__).resolve().parent` and
   `_HERE / "src"`.
2. Fix the 10 additional `PTH` violations surfaced when the rule is enabled
   (one `os.path.expanduser` → `Path.home()`, four `os.replace` →
   `Path.replace`, two `os.path.getsize` → `Path.stat().st_size`, one
   `os.path.splitext` → `Path.suffix`, two `os.symlink` → `Path.symlink_to`,
   one builtin `open()` → `Path.open()`).
3. Add `"PTH"` to `[tool.ruff.lint] select` in `pyproject.toml`.
4. Add `"PTH"` to the existing per-file ignores for `python/**`,
   `compat/python-vmaf/**`, `compat/python-vmaf/resource/**`, and
   `testdata/**` — same policy as the other style-family suppressions.
5. Suppress `PTH207` (one site) in `scripts/ci/agent-eligibility-precheck.py`
   with an inline `noqa` + justification: `glob.glob` is the correct
   primitive when the input is a user-overridable, fully-arbitrary glob
   string (absolute, multi-wildcard) — `Path.glob` would require parsing the
   user-supplied glob to split out a base directory.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix only the 2 named shim files; do not enable `PTH` | Smallest diff; matches literal task scope | No regression guard — the same pattern would silently reappear in the next copy-paste of a shim | Defeats the modernization's purpose; the guard is the point |
| Enable `PTH` and leave the 10 surfaced violations as `noqa` debt | Even smaller diff | 10 inline suppressions across 7 files is more debt than fixing them mechanically would have cost; obscures the intent of the rule | Cleanups are 1–2 lines each, mechanical, and locally tested — fixing inline costs less than the debt explanation |
| Force `PTH` onto Netflix upstream too | Maximum consistency | Creates rebase noise on every upstream sync; Netflix style is explicitly not our canon (ADR-0100) | Same policy as the other style-family per-file ignores |

## Consequences

- **Positive**: every fork-owned Python file now uses `pathlib.Path` for
  filesystem ops; ruff catches regressions at commit time before they land.
- **Negative**: minor — one inline `noqa: PTH207` carries a paragraph of
  justification (the only case where pathlib is genuinely wrong for the job).
- **Neutral / follow-ups**: when the next PR touches a `python/**` or
  `compat/python-vmaf/**` file with a fork-local rewrite, consider lifting
  the `PTH` ignore for that specific file rather than the whole tree.

## References

- Modernization roadmap: `project_vmafx_phase4_language_modernization` memory.
- Ruff PTH ruleset: <https://docs.astral.sh/ruff/rules/#flake8-use-pathlib-pth>
- Per-file-ignore precedent for upstream style families: ADR-0100.
- Source: `req` (direct user direction: "replace last 2 os.path usages with
  pathlib.Path, then add ruff PTH rule to ban regression").
