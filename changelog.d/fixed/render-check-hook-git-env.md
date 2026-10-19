- **The pre-push hook accepts a correctly rendered render-only pull request.** A git hook exports
  `GIT_DIR` and the other repository-local variables of the checkout it runs in; the ADR-3295
  render-only check inherited them in its detached worktree, `scripts/docs/fragment-order.py` read
  another checkout's history and returned another landing order, and the ADR index of a head that
  `make docs-render-check` accepts on its own was refused. The check now runs with those variables
  unset.
