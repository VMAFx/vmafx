- **Symphony-inspired agent-dispatch infrastructure
  ([ADR-0355](../docs/adr/0355-symphony-agent-dispatch-infra.md),
  [Research-0091](../docs/research/0091-symphony-spec-review.md)).**
  Three thin in-repo artefacts ported from
  [openai/symphony](https://github.com/openai/symphony) §3.1 / §4.1:
  (1) `.claude/workflows/` directory with a typed-YAML-front-matter
  `_template.md` plus three task-specific instances
  (`codeql-alert-sweep.md`, `simd-port.md`,
  `feature-extractor-port.md`); (2) read-only Python tracker
  abstraction `scripts/lib/backlog_tracker.py` exposing
  `BacklogItem` / `BacklogTracker` / `GitHubTracker` over
  `.workingdir2/BACKLOG.md` and `gh` PR queries; (3) pre-dispatch
  eligibility gate `scripts/ci/agent-eligibility-precheck.py` that
  refuses to dispatch when the BACKLOG row is closed, a merged PR
  already mentions the scope, or another agent is in flight on the
  same scope (exit 0/1; verdicts on stderr in GitHub Actions
  `::error` format). Stdlib-only — no PyYAML, no Linear SDK, no
  Elixir runtime. Closes the two confirmed NO-OP dispatches from
  the 2026-05-09 session (`vmaf_tiny_v3` registry already shipped
  via PR #351; T7-5 NOLINT sweep already closed by PR #82 + PR
  #388). Operator docs:
  [`docs/development/agent-dispatch.md`](../docs/development/agent-dispatch.md).
