<!-- markdownlint-disable MD013 MD060 -->
# BUG-048 — Zed 1.18.1 project-workflow restoration

## Scope

Commit `3f4f28a5a` added a broad Zed workflow configuration. Commit
`196a572ab` then replaced `.zed/settings.json`, `.zed/tasks.json`, and
`.zed/debug.json` with older copies while making an unrelated cJSON change.
The replacement carried no explanation of the Zed removal. Later governance
work restored only three standards tasks, leaving the useful development
workflows absent.

This audit does not replay the 1.3.6 files. It identifies which capabilities
remain valid on the installed Zed release, restores only those capabilities,
and rejects settings and commands that are now ignored or stale.

## Exact-source evidence

The installed editor reports `Zed 1.18.1` at Git revision
`bebe92f469834a287f5a57ed78e8d51a918b8ada`. The official Zed repository was
checked out at that exact revision before changing the project configuration.

| Question | Exact 1.18.1 evidence | Consequence |
|---|---|---|
| Which schema parses `.zed/settings.json`? | `crates/settings/src/settings_store.rs` calls `parse_and_migrate_zed_settings::<ProjectSettingsContent>` for project settings. | The full user schema is not available to the repository file. |
| Are agent and ACP registry settings project fields? | `crates/settings_content/src/project.rs` defines `ProjectSettingsContent` without `agent` or `agent_servers`; the full `SettingsContent` in `crates/settings_content/src/settings_content.rs` contains them. | Remove both roots from `.zed/settings.json`; document them as optional user configuration. |
| Which useful fields remain project-valid? | `ProjectSettingsContent` includes language, LSP, DAP, context-server, edit-prediction, and worktree settings. | Restore those fields selectively. |
| What is the custom MCP shape? | The current MCP documentation and exact settings types use `command`, `args`, and optional `env`. | Do not restore `source: "custom"`. |
| Who owns external-agent auth and models? | Current external-agent documentation delegates authentication and model choice to each ACP agent. | Do not pin provider models or native-agent permission policy in the project. |

The current local ACP registry was also inspected. Its applicable registry IDs
are `claude-acp`, `codex-acp`, and `gemini`; they remain user-installed agents,
not project dependencies.

## Repository evidence

- ADR-1229 makes the Go `vmafx-mcp` executable the current MCP implementation;
  the Python `vmaf-mcp` server is a compatibility surface.
- The dev container mounts `/workspace` read-only and reserves `/probes` for
  writable probe/build output. Container build tasks must not create a build
  directory below `/workspace`.
- The current tuning command is `vmaf-tune compare --src ...`; the old direct
  `tools/vmaf-tune/vmaftune/compare.py` path and `--ref` / `--dis` interface do
  not exist.
- `scripts/dev/validate_scores.py`, `scripts/dev/regen_docs.py`, the
  retired numbered workspace root, and repository-local `.venv/bin/*`
  assumptions are not current entrypoints.
- ADR-0726 removed Vulkan, so old GLSL and Vulkan editor associations are not
  restored.

## Restoration

- `.zed/settings.json` now contains only project-applied settings: clangd,
  Meson/C++ associations, edit-prediction exclusions, file-scan exclusions,
  and a Docker-backed `vmafx-mcp` context server.
- `.zed/tasks.json` preserves the three standards tasks and restores
  exact-source container build/start/shell/fast-test workflows, MCP probing,
  current vmaf-tune comparison, golden/format/lint gates, active-file pytest,
  and atomic ADR claiming.
- `.zed/debug.json` restores CodeLLDB launch/attach/sanitizer scenarios,
  Debugpy for pytest and `vmaftune.cli`, and Delve for `cmd/vmafx-mcp`.
- `docs/development/ide-setup.md` owns the live contract. The dated 1.3.6 plan
  and Research-0729 are explicitly historical.
- `scripts/ci/tests/test_zed_project_config.py` rejects ignored user settings,
  stale paths and entrypoints, loss of the governance tasks, and drift in the
  documented contract.

## Alternatives considered

| Option | Result | Decision |
|---|---|---|
| Cherry-pick or copy the 1.3.6 configuration | Restores many commands quickly, but also restores ignored project-level agent settings, obsolete model pins, the old MCP schema/binary, deleted helpers, retired local state, and Vulkan assumptions. | Rejected. |
| Keep the minimal three-task configuration | Avoids schema risk, but leaves the unrelated silent replacement in effect and discards valid LSP, context-server, task, and debugger workflows. | Rejected. |
| Put agent registry, model, and permission policy back in project settings | Appears reproducible, but exact source proves those roots are outside `ProjectSettingsContent`; it also crosses the developer trust boundary. | Rejected. |
| Restore only fields and commands proved current, with a fail-closed regression | Preserves useful shared workflows without claiming control of user/agent policy. | Selected. |

No ADR is needed. This is a one-way repair to the installed editor's existing
schema and already-adopted repository decisions (ADR-0726, ADR-1229, and
ADR-1277), not a new architecture or policy choice.

## Red/green evidence

Before the restoration, the focused contract produced `4 failed in 0.03s`:
the project settings lacked the current contract, tasks and debugger scenarios
were absent, and the stale documentation still claimed authority. After the
selective restoration:

```text
$ python3 -m pytest -q scripts/ci/tests/test_zed_project_config.py
.....                                                                    [100%]
5 passed in 0.02s
```

## Sources

- [Exact Zed project-settings type](https://github.com/zed-industries/zed/blob/bebe92f469834a287f5a57ed78e8d51a918b8ada/crates/settings_content/src/project.rs)
- [Exact Zed project-settings parser](https://github.com/zed-industries/zed/blob/bebe92f469834a287f5a57ed78e8d51a918b8ada/crates/settings/src/settings_store.rs)
- [Exact Zed full user-settings type](https://github.com/zed-industries/zed/blob/bebe92f469834a287f5a57ed78e8d51a918b8ada/crates/settings_content/src/settings_content.rs)
- [Zed external agents](https://zed.dev/docs/ai/external-agents)
- [Zed Model Context Protocol](https://zed.dev/docs/ai/mcp)
- [Zed edit predictions](https://zed.dev/docs/ai/edit-prediction)
- [Zed tasks](https://zed.dev/docs/tasks)
- [Zed debugger](https://zed.dev/docs/debugger)
