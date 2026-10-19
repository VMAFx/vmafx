- Added the required check `DCO Sign-off`: every commit of a pull request must
  carry a `Signed-off-by:` line (`git commit -s`; `git rebase --signoff` fixes a
  branch). Renovate, Dependabot and GitHub Actions bot commits in their own pull
  requests and the release pull request are exempt, and pull requests created
  before the cutoff in `scripts/ci/dco-cutoff.txt` are grandfathered. Run it locally with
  `python3 scripts/ci/check-dco.py --base origin/master --head HEAD`. See
  `docs/development/dco.md` and ADR-2462.
