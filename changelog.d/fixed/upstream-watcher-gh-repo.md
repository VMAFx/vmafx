- **The upstream watcher can no longer file issues on Netflix/vmaf.**
  When `upstream-watcher.yml` added a remote named `upstream`, `gh` resolved its
  base repository to upstream instead of the fork. Every job now sets
  `GH_REPO: ${{ github.repository }}` at job level and passes `--repo`
  to every `gh issue` command so tracking issues never target upstream.
