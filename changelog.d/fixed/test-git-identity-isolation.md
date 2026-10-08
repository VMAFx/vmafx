- Tests: a test script that builds a scratch git repository no longer writes into
  the repository that runs it. A git hook exports `GIT_DIR` and `GIT_INDEX_FILE`; the
  fixtures of 22 scripts (`git init`, `git config user.*`, `git commit`) inherited them
  and wrote a fixture identity (`user.email = t@t`) into the shared `.git/config`, and
  commits and index entries into the caller's repository. They now drop the hook
  environment (`scripts/lib/clean-git-env.sh`, or the equivalent in Python and Node)
  and take the fixture identity from `GIT_AUTHOR_*` / `GIT_COMMITTER_*`.
  `scripts/ci/test_git_fixture_isolation.py` fails on a `git config user.*` write and
  runs every scratch-repository test script against a sentinel `GIT_DIR`.
