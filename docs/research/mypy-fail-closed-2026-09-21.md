<!-- markdownlint-disable MD013 -->

# Fail-closed Python type-check gate — 2026-09-21

## Question

Can VMAFx replace ADR-1261's merge-base delta with a complete, blocking mypy
gate under the Python version the repository actually runs, without excluding
legacy files or suppressing dependency errors?

## Evidence

The pre-change configuration targeted Python 3.10 while root metadata and hosted
CI required Python 3.14. It excluded `ai/src/`, ignored entire module families,
and let the hosted command succeed through `|| echo`. The local wrapper checked
only branch-owned paths and subtracted findings reproduced at the merge base.
That design made a clean result mean "no newly fingerprinted diagnostic", not
"the checked Python is valid".

Replaying strict mypy under Python 3.14 exposed four independent classes:

1. source roots with two possible module identities;
2. untyped optional dependencies and dynamic imports;
3. genuinely incomplete annotations, JSON values used without runtime
   narrowing, and public functions whose annotations contradicted their runtime
   validation;
4. test helpers that exercised invalid input through type-suppression comments.

Package-root invocations with `--explicit-package-bases` solve the first class
without dropping files. Published stub distributions plus narrow local `.pyi`
contracts solve the second. Runtime shape checks, explicit protocols, truthful
parameter types, and fully annotated helpers solve the last two. Negative tests
now cross dynamic boundaries with runtime checks or `setattr`/`getattr`, not
`type: ignore`.

## Result

The canonical runner enumerates tracked sources with `git ls-files`, validates
each path, groups them by import root, and runs strict mypy over all groups. An
empty scope, missing Git or mypy, missing tracked file, checker crash, or normal
diagnostic all returns non-zero. Its regression fixture plants inherited and
current findings and proves there is no filename argument or baseline path that
can narrow the result.

The migration also found runtime-adjacent defects: the Unix-socket peer check
compared a socket kind with an address family; JSON payloads were trusted before
nested indexing; several functions advertised narrower inputs than they already
validated; and script tests imported the same module under both package and bare
names. Those were repaired rather than hidden.

## Reproduction

```bash
python3 scripts/git-hooks/test-pre-push-mypy.py
python3 scripts/git-hooks/pre-push-mypy.py
rg -n 'type:\s*ignore|ignore_errors|ignore_missing_imports|disable_error_code' \
  ai scripts pyproject.toml
```

The final `rg` command must return no type-check suppression in the gate-owned
trees or configuration.

## Limits

This decision owns `ai/` and `scripts/`. Imports reached from those roots remain
normal mypy dependencies and therefore must also be clean, but the runner does
not independently enumerate unrelated Python packages elsewhere in the tree.
