#!/usr/bin/env sh
# POSIX sh strict mode: -e errexit, -u nounset. pipefail is not in POSIX so it
# is opt-in below only when the host shell supports it. Stops the script from
# silently swallowing a failing python invocation or an unset $1 typo.

set -eu
# pipefail is bash/ksh/zsh; gate the opt-in to avoid tripping pure /bin/dash.
# shellcheck disable=SC3040  # set -o pipefail is non-POSIX, intentional guard
(set -o pipefail 2>/dev/null) && set -o pipefail || true

if [ -z "${1:-}" ]; then
  pattern='*_test.py'
else
  pattern="$1"
fi

PYTHONPATH=python python3 -m unittest discover -v -s python/test/ -p "$pattern"
