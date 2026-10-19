#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/git-hooks/pre-push-mkdocs-strict.sh — pre-push mkdocs strict-mode gate.
#
# Detects whether the push touches docs/ or mkdocs.yml; skips entirely if it
# does not (non-docs pushes see no latency penalty). When docs are touched,
# runs `mkdocs build --strict` against a temporary output directory, prints
# the mkdocs WARNING and ERROR lines on failure, and exits non-zero to block
# the push.
#
# Never add --quiet: it raises the log level to ERROR, so the WARNINGs that
# --strict counts are never emitted and a build with broken anchors exits 0.
# scripts/ci/tests/test_pre_push_mkdocs_strict.py fails on that form.
#
# Rationale (ADR-0466): audit slice G found that 38 of 50 master pushes failed
# the docs.yml CI lane for trivial mkdocs strict-mode breakage (broken anchors,
# orphan pages, missing nav entries). Each CI round-trip costs ~6 minutes. A
# 5-second local pre-push check eliminates the entire failure mode.
#
# Bypass: SKIP=mkdocs-strict git push
#         (or the standard --no-verify flag which skips ALL pre-push checks)
#
# Requirements:
#   mkdocs (+ docs/requirements.txt) must be installed in the active Python env.
#   `pip install -r docs/requirements.txt` satisfies this. Missing mkdocs
#   blocks a selected docs push; direct non-doc invocations skip before
#   requiring the docs toolchain.
#
# See also: docs/development/pre-push-mkdocs-strict.md

set -euo pipefail

# ── Skip override ─────────────────────────────────────────────────────────────
if [[ "${SKIP:-}" == *mkdocs-strict* ]]; then
  echo "pre-push-mkdocs-strict: SKIP set — skipping mkdocs strict check." >&2
  exit 0
fi

# ── Repo root ─────────────────────────────────────────────────────────────────
if ! repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || [ -z "${repo_root}" ]; then
  exit 0
fi

# ── Detect whether the push touches docs/ or mkdocs.yml ──────────────────────
# The framework selects this hook from Git's pushed-file range. For a direct
# invocation, fall back to the local branch diff against origin/master.
touched_docs=0

if [ "${PRE_COMMIT_REMOTE_NAME+x}" = x ]; then
  # Selection already used the pushed ref, which may differ from HEAD.
  touched_docs=1
elif ! git rev-parse --verify origin/master >/dev/null 2>&1; then
  # No remote reference yet (first push) — run the check conservatively.
  touched_docs=1
else
  if ! base="$(git merge-base origin/master HEAD 2>/dev/null)"; then
    base=""
  fi
  if [ -n "${base}" ]; then
    # Consume the full diff before matching: grep -q can close a large pipe
    # early, making Git fail with SIGPIPE under pipefail and skipping docs.
    if ! changed_files="$(git diff --name-only "${base}..HEAD" 2>/dev/null)"; then
      touched_docs=1
    elif grep -qE '^(docs/|mkdocs\.yml)' <<<"${changed_files}"; then
      touched_docs=1
    fi
  else
    # Cannot compute merge-base — run conservatively.
    touched_docs=1
  fi
fi

if [ "${touched_docs}" -eq 0 ]; then
  exit 0
fi

# ── mkdocs availability ───────────────────────────────────────────────────────
if ! command -v mkdocs >/dev/null 2>&1; then
  echo "pre-push-mkdocs-strict: BLOCKED — documentation changed but mkdocs is missing." >&2
  echo "  Install: pip install -r ${repo_root}/docs/requirements.txt" >&2
  exit 1
fi

# ── Run mkdocs build --strict ─────────────────────────────────────────────────
tmp_site="$(mktemp -d)"
trap 'rm -rf "${tmp_site}"' EXIT

echo "pre-push-mkdocs-strict: docs/ touched — running mkdocs build --strict..." >&2

mkdocs_out="$(mktemp)"
trap 'rm -f "${mkdocs_out}"; rm -rf "${tmp_site}"' EXIT

set +e
mkdocs build --strict \
  --config-file "${repo_root}/mkdocs.yml" \
  --site-dir "${tmp_site}" \
  >"${mkdocs_out}" 2>&1
mkdocs_exit=$?
set -e

# --strict turns every WARNING into a failed build; count them as well, so a
# future flag that silences or downgrades them cannot pass a broken tree.
# grep -c prints the count and exits 1 when it is 0.
if ! warning_count="$(grep -cE '^(WARNING|ERROR)' "${mkdocs_out}")"; then
  warning_count=0
fi

if [ "${mkdocs_exit}" -ne 0 ] || [ "${warning_count}" -ne 0 ]; then
  cat >&2 <<'HEADER'

pre-push-mkdocs-strict: BLOCKED — mkdocs build --strict failed.
Fix the errors below before pushing (saves a ~6-minute CI round-trip):

HEADER
  if [ "${warning_count}" -ne 0 ]; then
    grep -E '^(WARNING|ERROR)' "${mkdocs_out}" >&2
  else
    cat "${mkdocs_out}" >&2
  fi
  cat >&2 <<'FOOTER'

Bypass (this check only): SKIP=mkdocs-strict git push
Bypass (all pre-push checks): git push --no-verify

See docs/development/pre-push-mkdocs-strict.md for troubleshooting.
ADR-0466: docs/adr/0466-mkdocs-strict-pre-push-hook.md
FOOTER
  exit 1
fi

echo "pre-push-mkdocs-strict: mkdocs build --strict passed." >&2

# Formulas (ADR-2705): compile every one with the vendored KaTeX. The script
# exits 3 when Node.js is missing, which blocks like any other failure.
if [ -f "${repo_root}/scripts/docs/check_math.py" ]; then
  if ! python3 "${repo_root}/scripts/docs/check_math.py" --site "${tmp_site}" >&2; then
    echo "pre-push-mkdocs-strict: BLOCKED — a formula does not compile with KaTeX." >&2
    exit 1
  fi
fi
exit 0
