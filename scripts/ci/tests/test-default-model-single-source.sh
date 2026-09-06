#!/usr/bin/env bash
# Regression test for scripts/ci/check-default-model-single-source.sh.
#
# A gate that only ever passes proves nothing. This exercises both directions:
# the clean tree passes, and each way of breaking the single-source rule is
# actually caught. Every case runs against a scratch clone so the working tree
# is never modified.
set -uo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
gate=scripts/ci/check-default-model-single-source.sh
fails=0

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# A scratch git repo containing the tracked tree, so `git grep` sees the files.
clone() {
  local dst="$work/$1"
  rm -rf "$dst"
  mkdir -p "$dst"
  git -C "$repo_root" archive --format=tar HEAD 2>/dev/null | tar -x -C "$dst" ||
    {
      cp -r "$repo_root"/. "$dst"/ 2>/dev/null
      rm -rf "$dst/.git"
    }
  # include not-yet-committed work so the test is useful pre-commit too
  git -C "$repo_root" diff HEAD --binary >"$work/wip.patch" 2>/dev/null || true
  git -C "$dst" init -q 2>/dev/null
  git -C "$dst" config user.email t@t
  git -C "$dst" config user.name t
  if [ -s "$work/wip.patch" ]; then
    git -C "$dst" apply "$work/wip.patch" 2>/dev/null || true
  fi
  git -C "$dst" add -A -f >/dev/null 2>&1
  git -C "$dst" commit -qm scratch >/dev/null 2>&1
  printf '%s\n' "$dst"
}

expect() { # $1=label $2=expected-exit $3=dir
  local out rc
  out=$(cd "$3" && bash "$gate" 2>&1)
  rc=$?
  if [ "$rc" -eq "$2" ]; then
    printf 'PASS: %s (exit %d)\n' "$1" "$rc"
  else
    printf 'FAIL: %s — expected exit %d, got %d\n' "$1" "$2" "$rc"
    printf '%s\n' "$out" | sed 's/^/      /'
    fails=$((fails + 1))
  fi
}

# 1. the tree as it stands must pass
d=$(clone clean)
expect "clean tree passes" 0 "$d"

# 2. a Go mirror that disagrees with the C header must fail
d=$(clone godrift)
sed -i 's/^const DefaultVersion = ".*"$/const DefaultVersion = "vmaf_v0.6.1"/' \
  "$d/pkg/model/default.go"
git -C "$d" commit -aqm drift >/dev/null 2>&1
expect "drifted Go mirror is caught" 1 "$d"

# 3. a Python mirror that disagrees must fail
d=$(clone pydrift)
sed -i 's/^DEFAULT_MODEL = ".*"$/DEFAULT_MODEL = "vmaf_4k_v0.6.1"/' \
  "$d/tools/vmaf-tune/src/vmaftune/defaultmodel.py"
git -C "$d" commit -aqm drift >/dev/null 2>&1
expect "drifted Python mirror is caught" 1 "$d"

# 4. a newly reintroduced hardcoded default must fail
d=$(clone hardcode)
printf '\nfunc reintroducedDefault() string {\n\tif true {\n\t\treturn "vmaf_v0.6.1"\n\t}\n\treturn ""\n}\n' \
  >>"$d/pkg/fast/pipeline.go"
git -C "$d" commit -aqm hardcode >/dev/null 2>&1
expect "new hardcoded default is caught" 1 "$d"

# 4b. a hardcoded default in ai/scripts/ must fail (verifying ai/ is no longer exempt)
d=$(clone aihardcode)
printf '\ndef _reintroduced_ai_default():\n    return "vmaf_v0.6.1"\n' \
  >>"$d/ai/scripts/extract_full_features.py"
git -C "$d" commit -aqm aihardcode >/dev/null 2>&1
expect "ai script hardcoded default is caught" 1 "$d"

# 5. a deliberate pin carrying the marker must NOT fail
d=$(clone pinned)
printf '\nfunc specMandatedModel() string {\n\treturn "vmaf_v0.6.1" // vmaf-model-pin: test fixture\n}\n' \
  >>"$d/pkg/fast/pipeline.go"
git -C "$d" commit -aqm pinned >/dev/null 2>&1
expect "marked deliberate pin is allowed" 0 "$d"

# 5b. the fallback spellings an adversarial review found the first regex missing.
# Each of these is a real way to substitute a literal default, and each was
# invisible to the gate before 2026-09-03; cli.py:122 actually shipped one.
for spelling in \
  'model = getattr(args, "m", "vmaf_v0.6.1")' \
  'model = opts.get("model", "vmaf_v0.6.1")' \
  'model = chosen or "vmaf_v0.6.1"'; do
  d=$(clone "pyform-$(printf '%s' "$spelling" | tr -cd '[:lower:]' | cut -c1-8)")
  printf '\n\ndef _reintroduced_default(args, opts, chosen):\n    %s\n    return model\n' \
    "$spelling" >>"$d/tools/vmaf-tune/src/vmaftune/score.py"
  git -C "$d" commit -aqm form >/dev/null 2>&1
  expect "python fallback form is caught: ${spelling}" 1 "$d"
done

d=$(clone goflag)
printf '\nfunc reintroducedFlagDefault(cmd *cobra.Command, s *string) {\n\tcmd.Flags().StringVar(s, "vmaf-model", "vmaf_v0.6.1", "model")\n}\n' \
  >>"$d/pkg/fast/pipeline.go"
git -C "$d" commit -aqm goflag >/dev/null 2>&1
expect "Go flag-registration default is caught" 1 "$d"

# 5c. a doc-comment naming the current default is NOT a hardcoded fallback and
# must not fail the gate, or the gate becomes unusable and gets switched off.
d=$(clone comment)
printf '\n// ResolveExample turns a name (e.g. "vmaf_v0.6.1") into a path.\n// Callers may pass "vmaf" or "vmaf_v0.6.1" interchangeably.\nfunc ResolveExample() {}\n' \
  >>"$d/pkg/fast/pipeline.go"
git -C "$d" commit -aqm comment >/dev/null 2>&1
expect "doc-comment naming the default is allowed" 0 "$d"

# 5d. forms an ANCHORED comment filter must still catch. An earlier unanchored
# filter treated any line with a '*' or ';' before the literal as a comment,
# which silently blinded the gate to `const char *model = "vmaf_v0.6.1";` — the
# single most idiomatic C spelling of a hardcoded default.
d=$(clone cptr)
printf '\nconst char *reintroduced_default(void)\n{\n    const char *model = "vmaf_v0.6.1";\n    return model;\n}\n' \
  >>"$d/core/tools/vmaf_vpl.c"
git -C "$d" commit -aqm cptr >/dev/null 2>&1
expect "C pointer declaration default is caught (not read as a comment)" 1 "$d"

d=$(clone semi)
printf '\ndef _two_statements(x):\n    y = 1; model = "vmaf_v0.6.1"\n    return y, model\n' \
  >>"$d/tools/vmaf-tune/src/vmaftune/score.py"
git -C "$d" commit -aqm semi >/dev/null 2>&1
expect "default after a semicolon is caught (not read as a comment)" 1 "$d"

d=$(clone ternary)
printf '\ndef _ternary(chosen):\n    return chosen if chosen else "vmaf_v0.6.1"\n' \
  >>"$d/tools/vmaf-tune/src/vmaftune/score.py"
git -C "$d" commit -aqm ternary >/dev/null 2>&1
expect "python ternary fallback is caught" 1 "$d"

d=$(clone getenv)
printf '\nimport os\n\n\ndef _from_env():\n    return os.getenv("VMAF_MODEL", "vmaf_v0.6.1")\n' \
  >>"$d/tools/vmaf-tune/src/vmaftune/score.py"
git -C "$d" commit -aqm getenv >/dev/null 2>&1
expect "os.getenv fallback is caught" 1 "$d"

# 5e. code that a comment heuristic used to swallow. There is no such heuristic
# any more; these pin that it stays gone.
d=$(clone deref)
printf '\nvoid reintroduced_deref(const char **dest)\n{\n    *dest = "vmaf_v0.6.1";\n}\n' \
  >>"$d/core/tools/vmaf_vpl.c"
git -C "$d" commit -aqm deref >/dev/null 2>&1
expect "pointer-dereference assignment is caught" 1 "$d"

d=$(clone macro)
printf '\n#define REINTRODUCED_FALLBACK(x) ((x) ? (x) : "vmaf_v0.6.1")\n' \
  >>"$d/core/tools/vmaf_vpl.c"
git -C "$d" commit -aqm macro >/dev/null 2>&1
expect "preprocessor-macro fallback is caught" 1 "$d"

# 5f. the further idioms a third review named.
for spelling in \
  'model = kwargs.pop("model", "vmaf_v0.6.1")' \
  'model = opts.setdefault("model", "vmaf_v0.6.1")' \
  "model = chosen or 'vmaf_v0.6.1'"; do
  d=$(clone "idiom-$(printf '%s' "$spelling" | tr -cd '[:lower:]' | cut -c1-9)")
  printf '\n\ndef _more_forms(kwargs, opts, chosen):\n    %s\n    return model\n' \
    "$spelling" >>"$d/tools/vmaf-tune/src/vmaftune/score.py"
  git -C "$d" commit -aqm idiom >/dev/null 2>&1
  expect "fallback idiom is caught: ${spelling}" 1 "$d"
done

d=$(clone strdup)
printf '\nchar *reintroduced_strdup(void)\n{\n    return strdup("vmaf_v0.6.1");\n}\n' \
  >>"$d/core/tools/vmaf_vpl.c"
git -C "$d" commit -aqm strdup >/dev/null 2>&1
expect "C strdup fallback is caught" 1 "$d"

# 5g. prose must still be allowed, including a Python docstring, which no
# comment heuristic could have handled.
d=$(clone prose)
printf '\n\ndef _documented():\n    """Return a name.\n\n    Callers usually pass "vmaf_v0.6.1" or "vmaf_4k_v0.6.1".\n    """\n    return None\n' \
  >>"$d/tools/vmaf-tune/src/vmaftune/score.py"
printf '\n// ResolveThing maps a name (e.g. "vmaf_v0.6.1") to a path.\nfunc ResolveThing() {}\n' \
  >>"$d/pkg/fast/pipeline.go"
git -C "$d" commit -aqm prose >/dev/null 2>&1
expect "docstring and doc-comment prose are allowed" 0 "$d"

# 6. removing the authoritative macro must fail loudly, not silently pass
d=$(clone nomacro)
sed -i '/^#define VMAF_DEFAULT_MODEL_VERSION /d' "$d/core/include/libvmaf/model.h"
git -C "$d" commit -aqm nomacro >/dev/null 2>&1
expect "missing authoritative macro is caught" 1 "$d"

if [ "$fails" -ne 0 ]; then
  printf '\n%d case(s) failed\n' "$fails"
  exit 1
fi
printf '\nall cases passed\n'
