- CI: required checks are routed by **measured impact** instead of
  pre-declared path filters (ADR-1140, ported from `lusoris/k8s` #4964).
  Every workflow hosting an aggregator-required check now always starts;
  inside each job `scripts/ci/plan-ci-impact.py` diffs the event's exact
  revisions (merge-base aware for PRs) against `.github/ci-impact.json` and
  gates the heavy steps on the affected surface — `c_core`, `python`, `ai`,
  `go`, `rust`, `docs`, `shell`, `actions`, `container`, plus the
  `golden_harness` / `tiny_ai` / `python_lint` closures. Unprovable changes
  (unknown root, delete/rename, CI-authority files, missing merge-base,
  non-linear push, over-large diff) fall back to running everything. A
  `docs/`- or `renovate.json`-only PR now finishes the required set in
  about a minute instead of ~25 minutes of matrix, sanitizer, cppcheck and
  CodeQL time; the three divergent `paths-ignore` lists are gone; a
  contract test keeps the map in step with the tree. Non-required
  workflows keep their filters for now.
