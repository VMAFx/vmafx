- **The Gitleaks check scans only the commit it checked out.** It ran
  `git log --all` over a full-history checkout, so a finding on any branch in
  the repository, including a commit a force-push had already replaced,
  failed every other open pull request. Each run now scans the history of
  its own `HEAD`: on a pull request, master plus the PR's commits.
