# shellcheck shell=bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Source this at the top of every test script that builds a scratch git
# repository. A git hook (pre-commit, pre-push) exports GIT_DIR and
# GIT_INDEX_FILE; with those set, `git init`, `git config` and `git commit`
# inside a scratch directory act on the CALLER's repository, so a fixture
# identity (`user.email = t@t`) lands in the shared .git/config of the real
# checkout and every worktree. This file drops the hook environment, ignores
# the system and user configuration, and supplies the fixture identity through
# the environment, so no fixture has to run `git config user.*` at all.

# shellcheck source=scripts/lib/drop-git-env.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/drop-git-env.sh"

export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0=init.defaultBranch
export GIT_CONFIG_VALUE_0=master
export GIT_AUTHOR_NAME="Fixture"
export GIT_AUTHOR_EMAIL="fixture@example.invalid"
export GIT_COMMITTER_NAME="Fixture"
export GIT_COMMITTER_EMAIL="fixture@example.invalid"
