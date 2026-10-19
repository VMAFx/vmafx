# shellcheck shell=bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Source this before a script runs git on a repository other than the caller's:
# a scratch repository, a clone into a temporary directory. A git hook
# (pre-commit, pre-push) exports GIT_DIR and GIT_INDEX_FILE; with those set,
# `git init`, `git clone`, `git config`, `git add` and `git commit` act on the
# CALLER's repository whatever the working directory is. This file unsets every
# GIT_* variable and changes nothing else. A test fixture sources
# clean-git-env.sh instead, which adds a fixture identity and ignores the user
# and system configuration.

while IFS= read -r drop_git_variable; do
  unset "$drop_git_variable"
done < <(compgen -A variable GIT_)
unset drop_git_variable
