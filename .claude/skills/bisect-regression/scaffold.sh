#!/usr/bin/env bash
#
# scaffold.sh — driver for /bisect-regression.
#
# Usage: bash .claude/skills/bisect-regression/scaffold.sh \
#          --bad <sha> --good <sha> --predicate <test:NAME|score-delta:...|perf-threshold:...|netflix-golden>
#
# Materializes a `git bisect run` script at /tmp/bisect-predicate.sh, kicks off
# bisect, and renders a verdict to /tmp/bisect-regression-YYYY-MM-DD.md when the
# search completes. Sources lib/bisect-common.sh for the shared guards.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091  # sourced relative path resolved at runtime
source "$script_dir/../lib/bisect-common.sh"

bad=""
good=""
predicate=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bad)
      bad="$2"
      shift 2
      ;;
    --good)
      good="$2"
      shift 2
      ;;
    --predicate)
      predicate="$2"
      shift 2
      ;;
    -h | --help)
      grep -E '^# ' "$0" | sed 's/^# //'
      exit 0
      ;;
    *) bisect_die "unknown arg: $1" ;;
  esac
done

[[ -z "$bad" ]] && bisect_die "--bad <sha> required"
[[ -z "$good" ]] && bisect_die "--good <sha> required"
[[ -z "$predicate" ]] && bisect_die "--predicate <type[:arg…]> required"

bisect_require_clean_tree
bisect_assert_two_revs "$good" "$bad"

stash_ref=$(bisect_stash_push) || true
# shellcheck disable=SC2064  # we want $stash_ref expanded NOW, not at trap time
trap "bisect_stash_pop '$stash_ref'; git bisect reset >/dev/null 2>&1 || true" EXIT

# Materialize the predicate runner. Body matches the workflow in SKILL.md:
# build CPU only, evaluate predicate, exit 0/1/125.
predicate_script="/tmp/bisect-predicate.sh"
cat >"$predicate_script" <<EOF
#!/usr/bin/env bash
set -uo pipefail

# Rebuild — if build fails, mark as SKIP (125) rather than BAD.
if ! ninja -C build >/tmp/bisect-build.log 2>&1; then
  if ! meson setup build --reconfigure -Denable_cuda=false -Denable_sycl=false \
        >>/tmp/bisect-build.log 2>&1 \
        || ! ninja -C build >>/tmp/bisect-build.log 2>&1; then
    exit ${BISECT_SKIP}
  fi
fi

# Evaluate predicate.
predicate="${predicate}"
case "\$predicate" in
  test:*)
    name="\${predicate#test:}"
    meson test -C build "\$name" >/tmp/bisect-predicate.log 2>&1 \\
      && exit ${BISECT_GOOD} || exit ${BISECT_BAD}
    ;;
  netflix-golden)
    make test-netflix-golden >/tmp/bisect-predicate.log 2>&1 \\
      && exit ${BISECT_GOOD} || exit ${BISECT_BAD}
    ;;
  score-delta:*|perf-threshold:*)
    # Real implementation invokes /cross-backend-diff or /profile-hotpath here.
    # The stub returns SKIP so the operator is reminded to fill it in rather
    # than silently mis-bisecting.
    echo "predicate '\$predicate' not implemented in stub; fill in /tmp/bisect-predicate.sh" >&2
    exit ${BISECT_SKIP}
    ;;
  *)
    echo "unknown predicate: \$predicate" >&2
    exit ${BISECT_SKIP}
    ;;
esac
EOF
chmod +x "$predicate_script"
bisect_log "predicate script: $predicate_script"

git bisect start "$bad" "$good"
git bisect run "$predicate_script" || true

first_bad=$(git bisect log | awk '/first bad commit:/ {print $NF}' | tail -n1)
if [[ -z "$first_bad" ]]; then
  bisect_warn "no first-bad commit identified — check $predicate_script"
  first_bad="(none — bisect did not converge)"
fi

out_path="/tmp/bisect-regression-$(date +%Y-%m-%d).md"
# shellcheck disable=SC2016  # backticks + fences are literal markdown
extra=$(printf '## Predicate\n\n`%s`\n\n## Bisect log\n\n```\n%s\n```\n' \
  "$predicate" "$(git bisect log)")
bisect_render_verdict "commit" "$first_bad" "$out_path" "$extra"
