<!-- markdownlint-disable MD013 -->

# 2039 — Why the code-scanning alerts never clear

**Date**: 2026-09-07
**Scope**: the 20 open alerts on the repository's Security tab.
**Outcome**: two mechanical causes identified and fixed; every remaining alert
given a written disposition
([ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md)).

## The scanners are not off

The first hypothesis to rule out. They are running:

```text
2026-09-07T06:52:38Z  CodeQL       ref=refs/heads/master   results=57
2026-09-07T06:41:11Z  Semgrep OSS  ref=refs/heads/master   cat=semgrep-registry  results=17
2026-09-07T06:41:03Z  Semgrep OSS  ref=refs/heads/master   cat=semgrep-local     results=0
```

`security-scans.yml` is `active`. `code-scanning/default-setup` reports
`state=not-configured`, which is correct and expected: the repo uses the
*advanced* workflow, and the two are mutually exclusive. That status is not a
sign of anything being disabled.

Alerts persist because of two mechanisms, neither of which is visible from the
code.

## Cause 1 — `nosemgrep` does not remove a result from the SARIF

Four alerts sit on code that is already correct and already annotated:

```python
# SHA-1 used as a non-security memoization cache key (func name + repr(args)).
# usedforsecurity=False explicitly indicates non-cryptographic role.
# nosemgrep: python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1
h = hashlib.sha1((str(original_func.__name__) + str(args)).encode(),
                 usedforsecurity=False).hexdigest()
```

Semgrep's documentation explains why the alert survives: a `nosemgrep` comment
"still generates findings records that are automatically set to **Ignored**
triage state, rather than excluding code from scanning entirely." The Ignored
state is a Semgrep-platform concept. This workflow does not use the platform —
it writes SARIF and hands it to `github/codeql-action/upload-sarif`. GitHub sees
a result and keeps the alert open.

**Consequence for the fork:** an in-code `nosemgrep` directive is documentation
for humans, not a suppression. Writing more of them will never close an alert.
Closing one requires either that the code stop matching the rule, that the rule
stop running (`semgrep --exclude-rule`), that the result be filtered out of the
SARIF before upload, or that a maintainer dismiss it in the UI.

## Cause 2 — `paths-ignore` is inert for compiled languages that are built

`.github/codeql-config.yml` lists `core/test` and `build` under `paths-ignore`,
and alerts appear on both anyway. GitHub's documentation gives the rule:

> You can use this option when you run the CodeQL actions on an **interpreted
> language** (Python, Ruby, and JavaScript/TypeScript) or when you analyze a
> compiled language **without building the code**.

The CodeQL job builds C/C++ with meson + ninja. Every translation unit the build
compiles is extracted and analysed, `paths-ignore` notwithstanding. For a built
language the only scope controls are what the build step compiles and the
`query-filters` rule-id list.

A second, independent defect compounded it. GitHub's glob rules make a bare
`build` match **only a top-level directory**:

> The filter pattern characters `?`, `+`, `[`, `]`, and `!` are not supported
> and will be matched literally. `**` characters can only be at the start or end
> of a line, or surrounded by slashes.

The fork's build trees are `core/build`, `core/build-cuda`, `core/build-all` —
none of which a bare `build` matches. That is why alert 1018 landed on
`core/build/meson-private/tmpu2z1n35e/testfile.c`, a probe file meson generates
to test compiler features. The entry is now `**/build`, `**/build-*`,
`**/builddir`, which does apply to the Python analysis and stops that class of
noise.

## What the scanners found that was real

One alert out of twenty pointed at an actual defect, and it was bigger than the
alert said. `py/unused-global-variable` flagged `_VALID_AOM_CTCS` and
`_VALID_NFLX_CTCS` in `mcp-server/.../server.py`. Pulling the thread found a
whole duplicated constant block:

| Name | Defined at | Values |
| --- | --- | --- |
| `_VALID_TINY_DEVICES` | 375 **and** 504 | identical |
| `_VALID_TINY_RESIZES` | 389 **and** 518 | identical |
| `_VALID_BACKENDS` | 391 **and** 523 | identical |
| `_VALID_PIXFMTS` | 392 **and** 521 | identical |
| `_VALID_BITDEPTHS` | 393 **and** 522 | identical |
| `_VALID_AOM_CTCS` / `_VALID_AOM_CTC` | 373 / 519 | identical, only the singular is read |
| `_VALID_NFLX_CTCS` / `_VALID_NFLX_CTC` | 374 / 520 | identical, only the singular is read |

CodeQL could only see the two whose *names* differ. The other five are shadowed
rebindings: the second definition silently wins, so adding a backend to the
block at line 391 would have changed nothing. The values happen to agree today,
so there is no live wrong behaviour — it is a trap, not a bug, and the kind that
surfaces months later as "I added it and it did not take effect".

## The generalisable points

1. **A quiet scanner and an ignored scanner look identical from the Security
   tab.** Check `code-scanning/analyses` for the default branch before assuming
   anything is off; `default-setup: not-configured` means the advanced workflow
   is in use, not that scanning is disabled.
2. **A suppression that lives in the code does not necessarily reach the
   platform that shows the alert.** Verify the mechanism end to end before
   writing a second annotation in the same style as the first that did not work.
3. **Config that looks like it excludes something may be inert.** `paths-ignore`
   under a built compiled language, and a glob that does not match at depth, are
   both silent — nothing warns that the entry has no effect.
4. **Follow an "unused variable" note upstream.** The interesting finding is
   rarely the unused name itself; here it was the five *shadowed* siblings the
   query could not flag.

## Open question for the maintainer

Nothing in this fork can close the "correct as written" alerts without either a
SARIF post-processing step (an in-tree, reviewed allowlist that filters results
before upload) or a manual dismissal. ADR-1222 lays out both and takes neither:
per the standing rule, agents analyse and fix, and the maintainer decides
dismissals.
