#!/usr/bin/env bash
# check-container-source.sh — is the dev container built from the code you think?
#
# CLAUDE.md rule 15 says to rebuild the container when "its image predates the
# last master sync". Timestamps do not actually answer that question. A build
# run right now against a checkout that is 28 commits behind produces an image
# newer than every commit and older than none, so a time comparison calls it
# fresh while it is missing everything the rebuild was for. That happened on
# 2026-09-06: the container was rebuilt specifically to pick up the GPU
# default-model fixes (#1307, #1312, #1324), the checkout was behind, and the
# resulting image contained none of them. It was caught only because a test
# file was missing; a GPU smoke run in that image would instead have reported
# confident, stale numbers.
#
# What actually decides the question is the revision of the build context, so
# that is what this script checks.
#
#   --pre-build [REF]   (default) The checkout is about to be used as a build
#                       context: fail if it is behind REF (default
#                       origin/master), or dirty in a path the image bakes in.
#   --image NAME        Ask an existing image what revision it was built from
#                       (reads /etc/vmafx-dev-source) and compare it to REF.
#   --no-fetch          Skip `git fetch`; compare against the remote-tracking
#                       ref as it already is. For offline use.
#
# Exit: 0 current; 1 stale or dirty; 2 cannot tell (unknown revision, no repo).
set -euo pipefail

mode=pre-build
image=""
ref="origin/master"
fetch=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pre-build)
      mode=pre-build
      shift
      ;;
    --image)
      mode=image
      image="${2:?--image needs an image name}"
      shift 2
      ;;
    --no-fetch)
      fetch=0
      shift
      ;;
    --ref)
      ref="${2:?--ref needs a git ref}"
      shift 2
      ;;
    -h | --help)
      sed -n '2,26p' "$0"
      exit 0
      ;;
    *)
      echo "check-container-source: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$repo_root" ]]; then
  echo "check-container-source: not inside a git repository" >&2
  exit 2
fi
cd "$repo_root"

# Paths whose content ends up inside the image. A change under any of these
# means the built image and the checkout can disagree in a way that matters.
# Kept in sync with the rebuild-trigger list in CLAUDE.md rule 15.
BAKED_PATHS=(core mcp-server ai tools/vmaf-tune dev model python)

if [[ "$fetch" -eq 1 ]]; then
  git fetch --quiet origin 2>/dev/null ||
    echo "check-container-source: WARNING: git fetch failed; comparing against the remote-tracking ref as-is" >&2
fi

if ! git rev-parse --verify --quiet "$ref" >/dev/null; then
  echo "check-container-source: cannot resolve ref '$ref'" >&2
  exit 2
fi
ref_sha="$(git rev-parse "$ref")"

if [[ "$mode" == "image" ]]; then
  # `docker run` rather than `inspect`: the marker is a file in the image, and
  # reading it proves the image can actually be started.
  if ! marker="$(docker run --rm --entrypoint cat "$image" /etc/vmafx-dev-source 2>/dev/null)"; then
    echo "check-container-source: $image has no /etc/vmafx-dev-source." >&2
    echo "  It predates this marker, so what it was built from cannot be established." >&2
    echo "  Rebuild it before trusting any measurement taken inside it." >&2
    exit 2
  fi
  image_sha="$(awk -F= '$1=="source_rev"{print $2}' <<<"$marker")"
  if [[ -z "$image_sha" || "$image_sha" == "unknown" ]]; then
    echo "check-container-source: $image records source_rev=${image_sha:-<empty>}." >&2
    echo "  The build did not receive VMAFX_SOURCE_REV, so its source is unknown." >&2
    echo "  Rebuild via dev/docker-compose.yml, which passes it." >&2
    exit 2
  fi
  if [[ "$image_sha" == "$ref_sha" ]]; then
    echo "check-container-source: OK — $image was built from $ref ($(git rev-parse --short "$ref_sha"))."
    exit 0
  fi
  if git merge-base --is-ancestor "$image_sha" "$ref_sha" 2>/dev/null; then
    behind="$(git rev-list --count "$image_sha..$ref_sha")"
    echo "check-container-source: STALE — $image was built from $(git rev-parse --short "$image_sha"), $behind commit(s) behind $ref." >&2
    echo "" >&2
    echo "  Commits it is missing that touch baked-in paths:" >&2
    git log --oneline "$image_sha..$ref_sha" -- "${BAKED_PATHS[@]}" | awk '{ print "    " $0 }' >&2
    echo "" >&2
    echo "  Rebuild before measuring anything inside it." >&2
    exit 1
  fi
  echo "check-container-source: $image was built from $(git rev-parse --short "$image_sha"), which is not an ancestor of $ref." >&2
  echo "  It is a different line of development, not simply older. Rebuild if you meant to test $ref." >&2
  exit 1
fi

# --pre-build: the working tree is about to become a build context.
head_sha="$(git rev-parse HEAD)"
status=0

if [[ "$head_sha" != "$ref_sha" ]] && git merge-base --is-ancestor "$head_sha" "$ref_sha"; then
  behind="$(git rev-list --count "$head_sha..$ref_sha")"
  echo "check-container-source: STALE CONTEXT — HEAD is $behind commit(s) behind $ref." >&2
  echo "  Building now bakes $(git rev-parse --short "$head_sha"), not $(git rev-parse --short "$ref_sha")." >&2
  missing="$(git log --oneline "$head_sha..$ref_sha" -- "${BAKED_PATHS[@]}")"
  if [[ -n "$missing" ]]; then
    echo "" >&2
    echo "  Commits you would NOT get, in paths the image bakes in:" >&2
    awk '{ print "    " $0 }' <<<"$missing" >&2
  fi
  echo "" >&2
  echo "  Fix: git merge --ff-only $ref" >&2
  status=1
fi

dirty="$(git status --porcelain --untracked-files=no -- "${BAKED_PATHS[@]}")"
if [[ -n "$dirty" ]]; then
  echo "check-container-source: uncommitted changes in baked-in paths:" >&2
  awk '{ print "    " $0 }' <<<"$dirty" >&2
  echo "  The image will contain them. That is fine for a local experiment and" >&2
  echo "  wrong for anything you intend to publish or cite (ADR-1102)." >&2
fi

if [[ "$status" -eq 0 ]]; then
  echo "check-container-source: OK — HEAD $(git rev-parse --short "$head_sha") matches $ref; safe to build."
fi
exit "$status"
