<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2012: Lefthook and the pre-commit framework on Windows hosts

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: Lusoris
- **Tags**: ci, tooling, workspace, agents, git-hooks, windows

## Context

Lefthook had never been installed on the Windows 11 workstation running agent checkouts, leaving local commits without `pre-commit` or `commit-msg` checks. When lefthook was first installed on Windows, several defects prevented the [ADR-1249](1249-praetor-governance-adoption.md) hook stack from passing in Git Bash:

1. **Lefthook unescaped execution**: On Windows, Lefthook v2.1.14 (`internal/run/controller/exec/exec_windows.go`) executes commands via `"<sh>" -c "<run>"` without escaping the script argument. The first double quote terminates the `-c` argument, resulting in syntax errors on multi-line blocks (`unexpected end of file from 'if' command`) and silent loss of arguments.
2. **Drive letter colon in `PATH`**: Lefthook passes Git Bash a working directory formatted as `C:/...`. When prepending `.venv/Scripts` to `PATH`, the colon in `C:` splits the PATH entry, breaking tool discovery (e.g. `reuse.exe`).
3. **Hook installer collision**: `scripts/githooks/install.py` treats any unknown hook file as a custom hook and refuses to run. Lefthook's `pre-commit` and `pre-push` shims caused `make install-hooks` to abort, preventing installation of the `commit-msg` (Conventional Commits) and `pre-rebase` (worktree drift guard) dispatchers ([ADR-1241](1241-worktree-hook-dispatch.md)).
4. **Agent hook file churn**: `lefthook uninstall` re-marshals `.claude/settings.json` and `.codex/hooks.json` with Go's `json.MarshalIndent` (sorted keys, 2-space indentation), dirtying the checkout even when no hooks are removed.
5. **Windows-specific toolchain and build requirements**:
   - `reuse` 6.2.0 cannot use `python-magic` on Windows and fails unless a pure-Python encoding detector (`charset-normalizer`) is installed.
   - `cmd/vmafx-node/bpf/` referenced `cilium/ebpf/link.Tracepoint`, which is unavailable on Windows, causing `go vet ./...` (run by lefthook's `govet` hook) to fail.
   - Text fixtures and scripts wrote CRLF line endings on Windows, failing byte-exact generator checks.
   - Path separators in `check-container-image-references.py` failed against `/`-keyed exceptions on Windows.

## Decision

1. **Quote-free lefthook delegations**: `lefthook.yml` keeps every `run:` command on a single line with zero double quotes and zero block scalars (`|`, `>`). Complex stage execution is moved to standalone shell scripts (`scripts/git-hooks/framework-hooks.sh`), which execute under `bash`.
2. **Path normalization**: `scripts/git-hooks/framework-hooks.sh` uses `cygpath -u` to convert Windows `C:/...` paths before prepending them to `PATH`. It supports `.venv/Scripts/pre-commit.exe` on Windows and `.venv/bin/pre-commit` on POSIX systems.
3. **Installer coexistence**: `scripts/githooks/install.py` recognises Lefthook shims (`call_lefthook run`) as owned by Lefthook and leaves them in place. The supported installation order is `lefthook install` followed by `make install-hooks`.
4. **Normalized agent hook settings**: `.claude/settings.json` and `.codex/hooks.json` are committed formatted with sorted keys and 2-space indentation matching Go's `json.MarshalIndent`.
5. **Cross-platform build hygiene**:
   - `requirements/locks/pre-commit.txt` installs `reuse[charset-normalizer]==6.2.0`.
   - `cmd/vmafx-node/bpf/` files referencing `cilium/ebpf` tracepoints carry `//go:build linux`.
   - Python fixture generators and doc scripts enforce `newline="\n"`.
   - `check-container-image-references.py` uses `path.as_posix()` for comparison.
   - Added `windows-hooks` CI job to `.github/workflows/standards-gate.yml` running on `windows-2025` to continuously verify lefthook pre-commit in a scratch clone.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single-line quote-free runner script (chosen) | Robust against Lefthook escaping bugs; easily tested; cross-platform | Adds `scripts/git-hooks/framework-hooks.sh` | Avoids fragile escaping workarounds in YAML |
| Escaping quotes inside `lefthook.yml` | No helper script | Unescaped execution in Lefthook v2.1.14 cannot be safely escaped without engine changes | Fragile and broken in upstream Lefthook |
| Move all hooks into `lefthook.yml` | One hook manager | Reverses ADR-1249 and loses ADR-1241 worktree drift guard | Unnecessary policy divergence |
| Run pre-commit directly without Lefthook on Windows | Simple | Bypasses HISS governance checks (`praetorctl audit`, `hiss coverage`) | Violates repository governance invariants |

## Consequences

- **Positive**: Windows developers and agent runners can run the full pre-commit and pre-push governance stack locally.
- **Positive**: Continuous CI coverage for Windows lefthook execution on `windows-2025`.
- **Negative**: Installation order is strictly `lefthook install` first, then `make install-hooks`.
- **Follow-up**: `scripts/githooks/tests/test_install.py` enforces the quote-free and agent-settings invariants via `LefthookBridgeTests`.

## References

- [ADR-1249](1249-praetor-governance-adoption.md) — Praetor governance adoption and Lefthook hook ownership.
- [ADR-1241](1241-worktree-hook-dispatch.md) — Worktree hook dispatchers.
- [Pre-commit hooks guide](../development/pre-commit-hooks.md) — Setup and usage documentation.
- Upstream Lefthook issue: `exec_windows.go` unescaped `sh -c` execution.
