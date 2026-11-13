- **Move the `.cursor/rules/vmafx-workspace.mdc` instructions into the canonical agent documents, so `praetorctl compile-context --verify` no longer refuses an instruction file outside `AGENTS.md`.**
  Workspace defaults (`.vscode/` tasks, clangd as the only C/C++ language server, `meson setup <build-dir> core`)
  merge into `docs/development/ide-setup.md`, where the setup steps already live. The agent-integration and
  hand-off rules move to a new "Agent integration contract" section of `docs/development/agent-reference.md`,
  which section 9 of `AGENTS.md` already links. `AGENTS.md` gains one pointer line and the hand-written
  `.mdc` is deleted, so Cursor reads the generated projection instead.
