<!-- markdownlint-disable MD060 -->
# Pre-commit hooks — framework (default) vs native (opt-in)

The fork ships **two** parallel implementations of the local
pre-commit gate. Both reach the same destination (well-formatted,
ruff-clean staged files); they differ in startup cost.

| Path                          | Default? | Install with                             | Per-commit overhead | Tools                                     | Source of truth                     |
| ----------------------------- | -------- | ---------------------------------------- | ------------------- | ----------------------------------------- | ----------------------------------- |
| Framework (`pre-commit` venv) | yes      | `make install-hooks`                     | ~3 s per hook       | full `.pre-commit-config.yaml` matrix     | `.pre-commit-config.yaml`           |
| Native bash                   | opt-in   | `VMAFX_NATIVE_HOOKS=1 make install-hooks` | ~0.4 s total         | `ruff check --fix`, `clang-format -i`, `shfmt -w` | `scripts/githooks/pre-commit.sh` |

Both paths share the same `pre-push` hook
(`scripts/git-hooks/pre-push`) — the PR-body deliverables validator
(ADR-0108) is installed independently of which `pre-commit` you
choose, and is always wired in by either install path.

CI continues to run the **framework** pre-commit against the full tree
(`pre-commit run --all-files`) on every PR. The native path is a
local-only optimisation; it does not affect what CI gates.

## Why two paths?

The pre-commit framework wraps every formatter in an isolated
`pre-commit`-managed virtualenv. For a four-formatter touch
(ruff + clang-format + shfmt + trailing-whitespace) the per-hook
startup adds up to ~12 s before the first file is even inspected.
That cost is amortised across the framework's full hook matrix
(file-content checks, JSON / YAML validators, gitleaks, semgrep,
…) — but for the typical contributor's "fix one Python typo"
commit it dominates the wall clock.

The native bash path skips the venv wrap and shells out directly
to the formatter binaries on PATH. It implements only the three
formatters that contributors hit on every commit
(`ruff check --fix`, `clang-format -i`, `shfmt -w`). Everything
else the framework runs — gitleaks, semgrep, conventional-commits
checks, the agent-worktree-drift guard, copyright header
validation — stays in CI, where startup cost is amortised across
the full tree.

## When to pick which

Use the **framework** (default) if you:

- Want the full hook matrix locally (catch gitleaks / semgrep
  findings before push).
- Don't mind the per-commit cost (slower commits, faster CI
  agreement).
- Want the same hook surface as CI (no risk of "framework caught
  it, native missed it").

Use the **native** path (opt-in) if you:

- Make many small commits per session (the cumulative startup
  cost matters).
- Have ruff / clang-format / shfmt already installed and pinned
  on PATH (no surprises from framework version drift).
- Are comfortable that gitleaks / semgrep / conventional-commits
  will be caught by CI on push rather than locally.

## How to switch

Either path is idempotent: re-running `install-hooks` (or
`hooks-install` for the legacy alias) replaces the prior
`.git/hooks/pre-commit` symlink without touching any other hook.

```bash
# Switch to native:
VMAFX_NATIVE_HOOKS=1 make install-hooks

# Switch back to framework:
make install-hooks
```

Existing non-symlink `pre-commit` or `pre-push` hooks (e.g. a
contributor's hand-rolled wrapper) are preserved as
`pre-commit.local-backup` / `pre-push.local-backup` rather than
silently overwritten — both paths share this safety net.

## Native hook contract

`scripts/githooks/pre-commit.sh` collects staged files via

```bash
git diff --cached --name-only --diff-filter=ACM
```

and dispatches them to the matching formatter based on extension
and path prefix (mirroring the framework hook's scope):

- `*.py` under `ai/`, `scripts/`, `tools/`, `python/` → `ruff check --fix`
- `*.c` / `*.h` / `*.cpp` / `*.hpp` / `*.cc` / `*.cu` / `*.cuh`,
  excluding `subprojects/` and `core/test/data/` → `clang-format -i`
- `*.sh` / `*.bash` → `shfmt -w -i 2 -ci`

Each formatter is conditional on the binary being present on PATH.
A missing tool is logged as a one-line notice on stderr and does
**not** block the commit (graceful degrade); install the tool
locally if you want the gate to fire.

Files that the formatter rewrites are `git add`-ed back so the
commit picks up the autofix in the same operation, and a summary
line is printed:

```text
[pre-commit] auto-fixed and re-staged 3 file(s): ruff=2 clang-format=1
```

Bypass either path with `git commit --no-verify` (standard escape
hatch).

## Background

See [ADR-0924](../adr/0924-native-pre-commit-hooks.md) for the
decision context and the
[research digest](../research/0924-native-pre-commit-hooks.md) for
the cost-of-startup measurements and rejected alternatives
(replace the framework outright, write a Go helper, etc.).
