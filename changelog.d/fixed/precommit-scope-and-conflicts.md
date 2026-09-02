- markdownlint no longer lints golden fixtures as prose. The exclude pattern was
  `^testdata/`, anchored at the repository root, so fixture trees under
  `pkg/*/testdata/` — the benchmark renderer's byte-exact expected Markdown, for
  one — were treated as documentation. Broadened to `(^|/)testdata/`, which is
  what the accompanying comment ("fixture trees we don't author") already
  described. It stayed hidden because both the hook and `make lint-md` scope to
  changed files.
- `tools/vmaf-tune/tests/test_codec_adapter_av1_videotoolbox.py` no longer makes
  isort and ruff fight. Every import in that module follows a `sys.path.insert`,
  so E402 applies file-wide, but it was suppressed with per-line `# noqa: E402`
  comments. isort 9 rewrites import statements and mangles those, at one point
  producing `# noqa: E402  # noqa: E402; noqa: E402` on one line while dropping
  the comment from another — which then failed ruff. Replaced with a single
  file-level `# ruff: noqa: E402`, which no import reordering can disturb.
- `cmd/vmafx-tune/AGENTS.md` invariants are numbered sequentially again. Three
  of the parallel `vmafx-tune` ports each appended invariants numbered 13–17, and
  the union-merge that reconciled them (PR #1153) kept all three blocks, so the
  file listed 13–17 three times over. Renumbered 1–25.
