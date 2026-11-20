- Agents: `.config/lane-brief.yaml` makes the lane launcher refuse a vmafx review
  brief that lacks `Write nothing in the worktree` or `return: final message only`,
  so a review job can no longer spend its run and then end in the rc 79 worktree
  guard (ADR-3553).
