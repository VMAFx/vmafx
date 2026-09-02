<!-- markdownlint-disable MD060 -->
# Research-0729 — Zed 1.3.6 config refresh + agent_servers wiring

## Problem

The repo's `.zed/` config and the
[`docs/development/zed-migration-plan-2026-05-19.md`](../development/zed-migration-plan-2026-05-19.md)
plan were written against Zed 1.2.6. Zed 1.3.5 (2026-05-20) and 1.3.6
(2026-05-21) shipped seven new AI-side knobs the project config does not
exercise, and changed the `tool_permissions` schema to regex-based
`always_allow` / `always_deny` / `always_confirm` rules. The existing
config keeps working (Zed is lenient about both shapes), but a teammate
opening the repo on Zed 1.3.6 sees:

- A single `default_model` driving every AI surface (panel, inline,
  commit messages, thread summaries, subagents) — Sonnet 4.5 routes the
  inline-rewrite path even though Haiku 4.5 finishes those edits twice
  as fast and at a small fraction of the subscription cost.
- Only 3 of the 15 `vmaf-mcp` tools auto-allowed; the other 12 prompt
  per call even when they're catalog reads.
- No `agent_servers` block, so the Claude / Codex / Gemini ACP adapters
  fall to the teammate's global config and the team has no shared pin.
- Edit-prediction provider can currently send `python/vmaf/resource/`,
  `python/test/resource/`, `.corpus/`, `.workingdir2/` paths and
  `*.yuv`/`*.onnx`/`*.pkl`/`*.parquet` content to the provider on
  keystrokes — there's no `disabled_globs` blocklist.
- `.zed/debug.json` only has CodeLLDB launch configs; Python tests
  (`ai/tests/`, `mcp-server/vmaf-mcp/tests/`) and the vmaf-mcp stdio
  server have no debugger entry point.
- `.zed/tasks.json` has 15 entries but no container-aware tasks despite
  [`CLAUDE.md §12 r15`](../../CLAUDE.md) calling the
  `vmaf-dev-mcp` container the default mental model for vmaf / vmaf-tune
  / ai / MCP work.

## Decision

Apply the audit-driven config refresh as one batched PR that touches
JSON + docs only — no C / Python / build changes.

### `.zed/settings.json`

- Add `agent.inline_assistant_model`, `agent.commit_message_model`,
  `agent.thread_summary_model` (all → Haiku 4.5);
  `agent.subagent_model` (→ Sonnet 4.5, explicit) per the 1.3.5
  release note.
- Convert `agent.tool_permissions.tools` to the regex-based
  `always_allow` / `always_confirm` form documented at
  <https://zed.dev/docs/ai/agent-settings> (2026-05-22).
- Collapse the 15 vmaf-mcp tools into four regex rules: read-only
  catalog (`list_.*`, `describe_.*`) + score/version/probe/compare
  always allowed; long-running `run_.*` and `eval_.*` always require
  confirmation.
- Add `agent_servers` block with `claude-acp` / `codex-acp` / `gemini`
  as registry agents so a fresh clone gets the same ACP adapters
  regardless of teammate global config.
- Add `edit_predictions.disabled_globs` blocklist covering `build/`,
  `subprojects/`, `python/{test,vmaf}/resource/`, `.corpus/`,
  `.workingdir{,2}/`, `model/`, `*.yuv`, `*.onnx`, `*.onnx.data`,
  `*.pkl`, `*.parquet`, `*.bin`.
- Add `*.hip`, `*.metal`, `*.mm` to the `C++` file-type list (kept the
  existing `*.cu` / `*.cuh` entries); add `meson.build` /
  `meson_options.txt` → `Python` as a stopgap until a Zed Meson
  extension is verified.
- Add `*.parquet` to `file_scan_exclusions` (cache hygiene; arrow
  files are large binary).
- Set `agent.play_sound_when_agent_done = "when_hidden"` — useful for
  long agent threads where Zed is not in focus.

### `.zed/tasks.json`

Add 12 tasks covering the gaps:

- Container-first workflows (rebuild dev-mcp image; build vmaf inside
  the container; meson test fast inside the container) — aligns with
  [`CLAUDE.md §12 r15`](../../CLAUDE.md).
- ADR helpers (`adr: claim next number` using `$ZED_SELECTED_TEXT` as
  the slug; `adr: list claimed`).
- Cross-backend correctness (`validate scores: all backends, Netflix
  normal pair`).
- Python sub-tree quick gates (`pytest: ai/tests`, `pytest: current
  file`, `ruff: check current file`).
- Doc gates (`mkdocs: build strict`, `regen docs`).
- Local PR gate (`deliverables-check vs current PR body`).
- `format-check` (non-mutating CI-equivalent).

### `.zed/debug.json`

Add 5 Debugpy / CodeLLDB entries:

- Debugpy on pytest current file
- Debugpy on `ai/tests`
- Debugpy on vmaf-mcp server (stdio)
- Debugpy on vmaf-tune compare
- CodeLLDB on `build-san/test/test_feature` with ASan + UBSan env

### `docs/development/zed-migration-plan-2026-05-22.md`

Refresh of the 2026-05-19 plan against live Zed 1.3.6 docs. Adds:
version-bump section, 1.3.5/1.3.6 feature deltas, the seven new
`agent.*` keys, the `tool_permissions` regex schema change, the
`agent_servers` registry pattern, snippets workaround note (Zed has no
project-local snippets path), container-task block, and a
`Codex/Gemini ACP availability` short note for the case where Claude
limit hits and we want to fan out work to other ACP agents.

The 2026-05-19 plan stays in tree as a dated snapshot (cited in
audit-trail terms). The 2026-05-22 doc is the current reference.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
|---|---|---|---|
| Leave Zed config as-is | Zero churn | Inline + commit-msg keep routing through Sonnet 4.5 (~3x cost); 12/15 MCP tools still prompt; no team-shared ACP pin | Rejected — direct cost + UX hit on every keystroke. |
| Only update `tool_permissions` schema | Smallest diff | Doesn't capture cost-saving model split or the disabled_globs leak risk | Rejected as incomplete. |
| Land model split + tool_permissions only; defer tasks/debug/docs | Two ~50-LOC PRs | Splits one logical refresh into two PRs against the "no tiny PRs" rule | Rejected. |
| Land settings + tasks + debug + plan refresh together (chosen) | One coherent ~300-LOC batched PR; fully covers the audit | Slightly bigger review surface, but JSON + docs only, no code | **Chosen.** |
| Also ship a project-local snippets directory in this PR | Same review pass | Zed has no project-local snippet path — needs symlink workaround. Separate concern; defer to a follow-up. | Deferred (Research-0730 candidate). |

## Validation

```bash
# Static validation of the JSON shape (Zed will tolerate but this catches typos)
.venv/bin/python -c "import json; json.load(open('.zed/settings.json'))"
.venv/bin/python -c "import json; json.load(open('.zed/tasks.json'))"
.venv/bin/python -c "import json; json.load(open('.zed/debug.json'))"

# Verify the deliverables-check task in the new tasks list is well-formed
BASE_SHA=$(git merge-base origin/master HEAD) HEAD_SHA=HEAD \
  scripts/ci/deliverables-check.sh < .workingdir2/pr-zed-config-1-3-6-refresh-body.md

# Live-doc spot check that Zed 1.3.6 is current and the
# `subagent_model` setting actually exists in 1.3.5+:
# https://zed.dev/releases (retrieved 2026-05-22)
# https://zed.dev/docs/ai/agent-settings (retrieved 2026-05-22)
```

Behavioural validation in Zed itself:

- Open the project, open a Python file, press `Ctrl+Enter` — confirm
  Haiku 4.5 is used (status bar shows model name when assistant fires).
- Open the agent panel, start a thread — confirm Sonnet 4.5 is used.
- In an agent thread, request `@vmaf-mcp` `list_models` — confirm no
  permission prompt (regex allow rule).
- Same thread, request `run_compare` — confirm a permission prompt
  fires (regex confirm rule).
- Trigger commit-message generation in the Git panel — confirm Haiku
  4.5 is used.

These are interactive checks; the JSON validation above is the
automated layer that runs in CI.

## Sources

- <https://zed.dev/docs/ai/external-agents> — ACP / agent_servers
  registry (retrieved 2026-05-22)
- <https://zed.dev/docs/ai/mcp> — context_servers / `source: "custom"`
  / tool permissions (2026-05-22)
- <https://zed.dev/docs/ai/agent-settings> — every `agent.*` key, the
  regex `tool_permissions` schema (2026-05-22)
- <https://zed.dev/docs/ai/inline-assistant> — `Ctrl+Enter`, model
  override (2026-05-22)
- <https://zed.dev/docs/ai/edit-prediction> — `disabled_globs`
  (2026-05-22)
- <https://zed.dev/docs/ai/rules> — first-match rules priority list
  (2026-05-22)
- <https://zed.dev/docs/tasks> — `$ZED_*` variables, hooks (2026-05-22)
- <https://zed.dev/docs/debugger> — DAP, CodeLLDB, Debugpy
  (2026-05-22)
- <https://zed.dev/docs/configuring-languages> — LSP per-language
  settings (2026-05-22)
- <https://zed.dev/docs/snippets> — JSON format, global-only path
  (2026-05-22)
- <https://zed.dev/docs/migrate/vs-code> — keybinding deltas,
  `zed: import vs code settings` (2026-05-22)
- <https://zed.dev/releases> — Zed 1.3.6 (2026-05-21) latest
  (2026-05-22)

Local source files: `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`
(15 tool count); `.workingdir2/zed-migration-audit-2026-05-22.md`
(local audit snapshot — gitignored, not in PR scope).
