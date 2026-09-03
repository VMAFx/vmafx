- **Every dependency PR failed the documentation gates because the classifier
  diffed the wrong range.** `scripts/ci/classify-dependency-pr.sh` compared
  `base_sha..head_sha` with two dots, but GitHub's `pull_request.base.sha` is the
  base branch tip as it was when the PR was created, not the current merge base.
  Once master moves — constantly, on this repo — a two-dot range reports every
  file merged since the branch point *on top of* the PR's own change. A Renovate
  PR touching only `deploy/helm/vmafx/values.yaml` was measured as touching 36
  files including `core/src/feature/adm_avx2.c`, so the classifier correctly
  concluded "bot PR touches source code" from incorrect input and withheld the
  exemption. The PR then failed `Deep-Dive Deliverables Checklist` and the
  aggregator, and no re-run could ever fix it because the range was recomputed
  the same way each time.

  The explicit-SHA path now resolves the real fork point with `git merge-base`
  (falling back to `base_sha` if that fails). The script's other branch already
  did this correctly, which is why the bug only bit PRs where CI supplies both
  SHAs — that is, all of them.

  The 19 existing cases in `scripts/ci/test-classify-dependency-pr.sh` all feed a
  precomputed `--diff` file and therefore never exercised the git-diff path at
  all; that is why this survived. Case 20 builds a real repository where the base
  advances with a source-file commit after the PR branches, and asserts the
  helm-only bot PR is still exempt. Verified to fail (19 passed / 1 failed)
  against the unfixed script and pass (20 / 0) with it.
