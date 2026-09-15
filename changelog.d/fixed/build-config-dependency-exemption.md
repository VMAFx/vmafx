- Recognize the root `build-config.env` as a dependency manifest for bot PRs,
  including updates to its Dockerfile mirrors. Human changes on non-bot
  branches, mixed source changes, nested build configs, and unrelated env
  files retain the documentation gates.
