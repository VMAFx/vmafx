- The praetor governance engine moves from `f41e74d` to `6c772713a133`
  (ADR-1351), the newest praetor commit whose own CI is green. Its HISS
  scanners now find 244 existing issues the old engine did not measure: 140
  Python functions over 60 lines, 61 process exits from library code in
  Python, Go and Rust, 36 recursive Python functions and 7 Go calls without a
  deadline. The debt baseline records them, so the ratchet starts from 378
  entries instead of 182. The move refreshes the compiled agent context, the
  README governance block, the Paperclip harness, the branch ruleset template
  (now for `master`), the devcontainer's vendored praetor source and the
  documentation gate's locked files; the devcontainer keeps its
  `vmafx-dev-mcp` base image. `make verify-all` now also runs praetor's
  Documentation Governance gate (`make docs-lint` and `make docs-figures`),
  which needs Node.js 24. `.standards.yaml` declares the text register for
  every surface (agent-only text is `internal`, the terse `caveman` form).
  The audit in the git hooks passes `--offline`, which saves about 40 seconds
  per commit and push. Every workstation's `praetorctl` has to move to the new
  pin when this merges; the two engines do not read each other's trees
  ([CI guide](docs/development/ci.md#moving-the-praetor-pin)).
