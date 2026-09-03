#!/usr/bin/env bash
# Enforce that the fork's default VMAF model is defined in exactly one place.
#
# The authoritative definition is VMAF_DEFAULT_MODEL_VERSION in
# core/include/libvmaf/model.h. Components that link libvmaf read it at runtime
# through vmaf_default_model_version(); components that deliberately do not link
# libvmaf (most of the Go tree, the Python tools) carry a mirror constant. This
# gate fails when a mirror drifts from the header, and when any component
# reintroduces its own hardcoded default.
#
# It does NOT ban the model name everywhere. Naming a specific model on purpose
# is normal and correct in: the Netflix golden test harness (its assertions are
# pinned to v0.6.1 by ADR-0024 and must never move), the AOM CTC preset (the
# spec mandates v0.6.1), model-name lookup tables, and documentation.
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

header=core/include/libvmaf/model.h
fail=0

note() { printf '%s\n' "$*" >&2; }
bad() {
  note "error: $*"
  fail=1
}

# ---------------------------------------------------------------- authority --
if [ ! -f "$header" ]; then
  note "error: $header not found; cannot determine the authoritative default"
  exit 1
fi

authoritative=$(sed -n 's/^#define VMAF_DEFAULT_MODEL_VERSION "\(.*\)"$/\1/p' "$header")
if [ -z "$authoritative" ]; then
  note "error: $header does not define VMAF_DEFAULT_MODEL_VERSION"
  note "       That macro is the single source of truth for the fork's default model."
  exit 1
fi
count=$(grep -c '^#define VMAF_DEFAULT_MODEL_VERSION ' "$header" || true)
if [ "$count" -ne 1 ]; then
  bad "$header defines VMAF_DEFAULT_MODEL_VERSION $count times; expected exactly 1"
fi
note "authoritative default model: $authoritative  ($header)"

# ------------------------------------------------------------------ mirrors --
# Each mirror is "<file>:<regex capturing the value>". A mirror exists only
# because its language cannot read the C header; it must agree with it exactly.
check_mirror() {
  local file=$1 sedexpr=$2 label=$3
  if [ ! -f "$file" ]; then
    bad "mirror missing: $file ($label)"
    return
  fi
  local value
  value=$(sed -n "$sedexpr" "$file" | head -1)
  if [ -z "$value" ]; then
    bad "$file: could not read the $label mirror value"
    return
  fi
  if [ "$value" != "$authoritative" ]; then
    bad "$file: $label mirror is \"$value\" but $header says \"$authoritative\""
    note "       Update the mirror to match the header, never the other way round."
  fi
}

check_mirror pkg/model/default.go \
  's/^const DefaultVersion = "\(.*\)"$/\1/p' "Go DefaultVersion"

check_mirror tools/vmaf-tune/src/vmaftune/defaultmodel.py \
  's/^DEFAULT_MODEL = "\(.*\)"$/\1/p' "vmaf-tune DEFAULT_MODEL"

check_mirror tools/vmaf-roi-score/src/vmafroiscore/defaultmodel.py \
  's/^DEFAULT_MODEL = "\(.*\)"$/\1/p' "vmaf-roi-score DEFAULT_MODEL"

# ------------------------------------------------- unapproved hardcoded uses --
# A default is "hardcoded" when a component substitutes a literal model name
# because the caller supplied none. Those are what this gate is for.
# Paths where naming a model explicitly is correct, not a hidden default:
#   python/test, compat/python-vmaf  the Netflix golden harness; its assertions
#                                    are pinned to v0.6.1 by ADR-0024
#   core/src/model.c                 the built-in model registry — it exists to
#                                    name every model it embeds
#   ai/                              training-corpus metadata recording which
#                                    model produced a score; data, not a default
#   docs, model, testdata, changelog documentation and fixtures
#   this script and its tests        they must contain the string to check it
allow_re='(^python/test/|^compat/python-vmaf/|^docs/|^model/|(^|/)testdata/|^changelog\.d/|^CHANGELOG\.md|^ai/|^core/src/model\.c:|^scripts/ci/check-default-model-single-source\.sh:|^scripts/ci/tests/|^core/include/libvmaf/model\.h:|^pkg/model/default\.go:|^tools/vmaf-tune/src/vmaftune/defaultmodel\.py:|^tools/vmaf-roi-score/src/vmafroiscore/defaultmodel\.py:|^dev/scripts/|^core/tools/cli_parse\.c:)'

# Tests pin models deliberately; a test that says "score with v0.6.1" is
# asserting behaviour, not choosing a default on a user's behalf.
test_re='(_test\.go|/tests?/|(^|/)test_[^/]*\.(c|cpp|py)$|_test\.py$)'

# Patterns that mean "this is the fallback when nothing was asked for".
#
# A blanket search for the literal is NOT usable here: 34 of its 35 hits are
# doc-comments ("e.g. \"vmaf_v0.6.1\"") and model-name lookup tables, which are
# legitimate. So each *default-substitution* form is matched explicitly. The
# getattr / .get / or / flag-registration forms were added after an adversarial
# review found a real hardcoded fallback the first three patterns missed
# (getattr(args, attr, "vmaf_v0.6.1") in tools/vmaf-tune/src/vmaftune/cli.py).
# If you add a new way to spell "fall back to a literal model", add it here and
# add a case to scripts/ci/tests/test-default-model-single-source.sh.
#
# There is deliberately NO "is this a comment?" heuristic. An earlier version had
# one and it was worse than the problem: anchored loosely it classified
# `const char *model = "vmaf_v0.6.1";` as a comment (the `*` of a pointer
# declaration) and blinded the gate; anchored tightly it still swallowed
# `*dest = "..."` and `#define FALLBACK ...`, and missed prose inside a
# docstring. Instead every pattern below requires real assignment/return/call
# syntax immediately around the literal, which prose never has.
#
# Known limits, accepted deliberately because `git grep` is line-oriented and
# these are not idioms anyone writes by accident: a literal split across lines
# (model =\n    "vmaf_v0.6.1"), and a literal built by concatenation
# ("vmaf_v0" + ".6.1").
M='["'"'"']vmaf_v0\.6\.1(neg)?["'"'"']'
default_use_re="(=[[:space:]]*${M}"
default_use_re="${default_use_re}|:[[:space:]]*${M}[[:space:]]*[,);]"
default_use_re="${default_use_re}|return[[:space:]]+${M}"
default_use_re="${default_use_re}|getattr\\([^)]*,[[:space:]]*${M}"
default_use_re="${default_use_re}|\\.get\\([^)]*,[[:space:]]*${M}"
default_use_re="${default_use_re}|[=:][^=]*\\|\\|[[:space:]]*${M}"
default_use_re="${default_use_re}|(=|return)[^=]*[[:space:]]or[[:space:]]+${M}"
default_use_re="${default_use_re}|StringVar\\([^)]*,[[:space:]]*${M}"
default_use_re="${default_use_re}|StringP?\\([^,]*,[[:space:]]*${M}"
default_use_re="${default_use_re}|getenv\\([^)]*,[[:space:]]*${M}"
default_use_re="${default_use_re}|\\.pop\\([^)]*,[[:space:]]*${M}"
default_use_re="${default_use_re}|setdefault\\([^)]*,[[:space:]]*${M}"
default_use_re="${default_use_re}|value_or\\([[:space:]]*${M}"
default_use_re="${default_use_re}|strdup\\([[:space:]]*${M}"
default_use_re="${default_use_re}|[Oo]r\\([^)]*,[[:space:]]*${M})"
default_use_re="${default_use_re}|(=|return)[^=]*[[:space:]]else[[:space:]]+${M}"

# Drop lines whose only match is inside a comment. Documentation that names the
# current default ("e.g. \"vmaf_v0.6.1\"") is not a hardcoded fallback, and
# banning it would make the gate unusable.

offenders=$(git grep -nIE "$default_use_re" -- . 2>/dev/null |
  grep -vE "$allow_re" |
  grep -vE "$test_re" || true)

# A line may pin a model deliberately by carrying the marker below, which
# forces the author to say why in the source itself rather than in a path list.
# The AOM CTC preset uses it: the CTC specification requires that exact model,
# so it is not a default and must never follow the fork's default.
offenders=$(printf '%s\n' "$offenders" | grep -v 'vmaf-model-pin:' || true)

if [ -n "${offenders//[[:space:]]/}" ]; then
  note ""
  note "error: these sites hardcode a default model instead of deriving it:"
  printf '%s\n' "$offenders" | sed 's/^/       /' >&2
  note ""
  note "       C/C++ linking libvmaf : use VMAF_DEFAULT_MODEL_VERSION"
  note "       other languages       : use the mirror (Go: pkg/model.DefaultVersion)"
  note "       pinning a model on purpose (a spec-mandated preset, a golden"
  note "       assertion) is fine: put a 'vmaf-model-pin: <reason>' comment on the"
  note "       line so the reason lives next to the code."
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  note ""
  note "default-model single-source check FAILED"
  exit 1
fi

note "default-model single-source check passed"
