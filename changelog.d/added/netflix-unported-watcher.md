- **Daily Netflix/vmaf unported-commit watcher.** A daily CI workflow
  (`.github/workflows/upstream-watcher.yml`) detects commits on Netflix/vmaf
  `master` after the fork's recorded parity pin that have no disposition in
  `scripts/upstream-watcher/netflix-dispositions.tsv`, and opens deduplicated
  tracking issues for fork triage. The companion script
  `scripts/upstream-watcher/check_netflix_unported.py` enables local checking
  ([Upstream watchers](docs/development/upstream-watchers.md)).
