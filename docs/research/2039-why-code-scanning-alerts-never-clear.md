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

## Addendum 2026-09-22 — the dismissal is what holds, and it does not survive code motion

Cause 1 above was inferred from Semgrep's documentation. It is now measured, and
the measurement is sharper than the inference.

Semgrep's SARIF does carry the `nosemgrep` directive: a suppressed result is
emitted with `"suppressions": [{"kind": "inSource"}]` (reproduced locally with
semgrep 1.177.0 on `core/test/test_windows_cuda_compiler_discovery.py`, the same
three registry packs `security-scans.yml` runs). GitHub ingests that result and
opens the alert anyway — `most_recent_instance.state` is `open`. Two independent
observations:

- Alert **1062** was opened on 2026-09-15 and the `nosemgrep` for it landed in
  `fbab6d65c` on 2026-09-16. Its dismissal comment reasons that "the alert
  predates it", which assumed the directive would have prevented it.
- Alert **1240** was opened on 2026-09-22 from a tree that had carried that same
  directive for six days. It did not.

So the directive never suppressed anything on the Security tab; the **manual
dismissal** is the only thing that has ever closed one of these. That matters
because a dismissal is bound to the alert, and Semgrep OSS emits no
`partialFingerprints`, so GitHub matches an alert by location and snippet. The
HISS-21 refactor `50657c98f` lifted `_run_meson` out of a class body to module
level: same call, same `# noqa`, same `# nosemgrep`, but line 120 → 145 and
column 17 → 9. GitHub could not map it to the dismissed 1062 and minted 1240,
which fails the (non-required, per [ADR-0037](../adr/0037-master-branch-protection.md))
`Semgrep OSS` check as "1 new alert".

The finding itself is unchanged and pre-existing: master's copy of the file
produces the identical result under the identical config. Nor can the code stop
matching. The rule's only sanitizer is `shlex.quote(...)`, which is wrong for a
member of an argv **list** executed without a shell; taint propagates through
`shutil.which()` and `Path.resolve()` (both measured); and the taint source is
`MESON = os.environ.get("VMAFX_TEST_MESON") or shutil.which("meson")`, whose
value `core/test/meson.build` supplies so the test runs the same Meson that
launched it. Every channel a build system has for telling a test where a tool
lives — environment or argv — is a source for this rule.

That leaves exactly the two options ADR-1222 put to the maintainer, and this
addendum changes neither of them: the gating `Semgrep` job (local `.semgrep.yml`
rules, 0 findings tree-wide) stays green, `Semgrep OSS` stays a reporting
signal, and the dismissal remains the maintainer's call.

## Addendum 2026-09-22 (second) — the taint claim was wrong; the code could stop matching

The addendum above concluded that the Semgrep finding "cannot stop matching",
on the reasoning that "every channel a build system has for telling a test
where a tool lives — environment or argv — is a source for this rule". Half of
that is right and the conclusion drawn from it is wrong, so the alert was closed
by fixing the code, not by dismissing it.

Read from the rule itself rather than from its behaviour
(`https://semgrep.dev/c/r/python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args`),
the source set is exactly: `os.environ`, `os.environ.get`, `os.environb`,
`os.getenv`, `os.getenvb`, `sys.argv`, `sys.orig_argv`, and argparse / optparse
/ getopt results. The sole sanitiser is `shlex.quote(...)`.

Three consequences the earlier reading missed:

- **argv really is a source**, so passing the Meson path as a test argument
  would not have helped. That half of the claim holds.
- **`shutil.which("meson")` is not a source.** It reads `PATH` internally, but
  the rule matches syntax, not library behaviour, and a literal argument taints
  nothing. The earlier note that "taint propagates through `shutil.which()`"
  only holds when its *argument* is already tainted.
- **A value the interpreter derives about itself is not a source either.**
  `sys.executable` appears nowhere in the source set.

That last point is the fix. Meson ships as an ordinary Python package, and
`import('python').find_installation()` hands a test the interpreter Meson is
running under — measured directly: under `meson test` the child's
`sys.executable` is the venv Python that owns the `mesonbuild` package. So
`[sys.executable, "-m", "mesonbuild.mesonmain", "setup", ...]` runs *the same
Meson* the environment variable was there to name, with no hand-off from the
build system and no tainted value in the argv. `core/test/meson.build` no longer
sets `VMAFX_TEST_MESON`, and the module-level read of it is gone.

Measured with semgrep 1.177.0 over the packs `security-scans.yml` runs, with
`--disable-nosem` so no directive could mask the result: **1 finding before, 0
after**. The stale `nosemgrep` comment was deleted rather than moved — it had
never suppressed anything, as the first addendum established.

The generalisable point, which replaces "the code cannot stop matching": a taint
rule is a syntactic source list, and the question to ask is never "is this value
attacker-controlled in practice" but "does this expression match one of the
listed sources". Fetch the rule and read it before concluding a finding is
unfixable. The same check applied to CodeQL's `cpp/world-writable-file-creation`
(`DoNotCreateWorldWritable.ql`) showed it reads the literal mode argument of the
creating call, so moving `core/test/test_pelorus_interop.c` from
`fopen(path, "w")` to a descriptor opened `S_IRUSR | S_IWUSR` clears it — and
that one was not a false positive at all: the old form measures 0666 under
umask 000.
