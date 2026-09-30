- The praetor governance engine moves from `f41e74d` to `25451d8`
  (ADR-1351). Its HISS scanners now find 246 existing issues the old engine
  did not measure: 142 Python functions over 60 lines, 61 process exits from
  library code in Python, Go and Rust, 36 recursive Python functions and 7 Go
  calls without a deadline. The debt baseline records them, so the ratchet
  starts from 431 entries instead of 185. The move refreshes the compiled agent
  context, the README governance block, the Paperclip harness, the branch
  ruleset template (now for `master`) and the devcontainer's vendored praetor
  source; the devcontainer keeps its `vmafx-dev-mcp` base image.
  `make verify-all` now also runs praetor's Documentation Governance gate
  (`make docs-lint` and `make docs-figures`), which needs Node.js 24. The
  gate's bounds and style exclusions live in the `documentation` block of
  `.standards.yaml`. The predictor model cards, their generator template and
  two pages were fixed to pass it.
