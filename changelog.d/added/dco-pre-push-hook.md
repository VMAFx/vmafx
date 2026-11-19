- **DCO sign-off check before every push.** The `dco-sign-off` pre-push hook
  runs `scripts/ci/check-dco.py --merge-base origin/master` on the commits a push
  adds, so a commit that never passed the local commit-msg hook, such as a
  review bot's suggestion pulled from the pull request branch, stops locally
  instead of turning the DCO job red. See `docs/development/dco.md`.
