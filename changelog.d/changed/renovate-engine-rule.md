- **Renovate leaves the praetor engine alone.** `renovate.json` gains the
  praetor-managed engine rule, so Renovate opens no pull requests of its own
  for the praetor module or the `standardsctl` command. Engine updates arrive
  in one coordinated adoption pull request. The engine's
  `renovate-engine-bumps` audit checks that the rule is present.
