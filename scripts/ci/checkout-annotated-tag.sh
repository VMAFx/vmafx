#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Materialize a new, shallow checkout at the commit behind a release tag.
# Fetching the peeled commit avoids Git's noisy shallow-clone warning for
# annotated tags while recreating the local tag needed by version discovery.

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <remote> <tag> <new-destination>" >&2
  exit 2
fi

remote="$1"
tag="$2"
destination="$3"

if [[ ! "$tag" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
  echo "invalid tag name: $tag" >&2
  exit 2
fi
if [[ -e "$destination" || -L "$destination" ]]; then
  echo "checkout destination already exists: $destination" >&2
  exit 2
fi

# A hook caller's repository, index, object store, and global configuration
# must never affect this disposable checkout.
for git_variable in ${!GIT_@}; do
  unset "$git_variable"
done
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0
tag_git() {
  command git -c core.hooksPath=/dev/null -c commit.gpgsign=false \
    -c user.name='VMAFx tag checkout' -c user.email=tag-checkout@localhost "$@"
}

tag_ref="refs/tags/${tag}"
peeled_ref="${tag_ref}^{}"
remote_refs="$(tag_git ls-remote "$remote" "$tag_ref" "$peeled_ref")"
commit="$(awk -v ref="$peeled_ref" '$2 == ref { print $1; exit }' <<<"$remote_refs")"
if [[ -z "$commit" ]]; then
  commit="$(awk -v ref="$tag_ref" '$2 == ref { print $1; exit }' <<<"$remote_refs")"
fi
if [[ ! "$commit" =~ ^[0-9a-fA-F]{40}$ ]]; then
  echo "remote did not resolve tag $tag to a commit" >&2
  exit 1
fi

tag_git init --quiet "$destination"
tag_git -C "$destination" remote add origin "$remote"
tag_git -C "$destination" fetch --quiet --depth=1 --no-tags origin "$commit"
tag_git -C "$destination" checkout --quiet --detach FETCH_HEAD
actual="$(tag_git -C "$destination" rev-parse HEAD)"
if [[ "$actual" != "$commit" ]]; then
  echo "checkout mismatch for $tag: expected $commit, got $actual" >&2
  exit 1
fi
tag_git -C "$destination" tag "$tag" "$commit"
printf 'Checked out %s at %s\n' "$tag" "$commit"
