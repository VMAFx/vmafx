- **The local PR-body validator checks render-only pull requests again.** `scripts/ci/validate-pr-body.sh`
  hands `deliverables-check.sh` the sentinel `validator-head` as the head, and the render-only path of
  ADR-3295 checks the head out to run `make docs-render-check`; the checkout of a name that is no commit
  failed, so the pre-push hook refused every render-only pull request with "cannot check out
  validator-head". The validator's git shim now checks out the commit it runs on.
