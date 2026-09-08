<!-- markdownlint-disable MD013 -->
# Research-1245: Cppcheck branch budgets and exhaustive analysis

## Reproduced cause

Installed Cppcheck 2.21.1 returned exit 1 on the cleaned pdjson parser and its
regression tests solely because of `normalCheckLevelMaxBranches`. The same 14
actual parser/test/caller compilation commands returned exit 0 with
`--check-level=exhaustive`; only the informational checker inventory remained.
The canonical parser evidence is
`.workingdir2/evidence/pdjson-lint-20260908/manifest.json`.

The smallest retained control uses eight sequential conditional early returns.
Normal analysis reports its branch cutoff and fails; exhaustive analysis
finishes with no source diagnostics. Unlike the parser reproduction, this
control needs neither a native build nor generated headers. Mandatory existing
controls still reject real uninitialized C-member reads and broken C++
constructors using the production exhaustive arguments.

## Authoritative tool behavior and compatibility

- [Cppcheck 2.21.1 settings.cpp](https://github.com/danmar/cppcheck/blob/2.21.1/lib/settings.cpp)
  assigns normal analysis a four-branch forward budget. Exhaustive removes it,
  removes the function if-count limit, increases argument analysis and enables
  condition-expression analysis.
- [Cppcheck 2.21.1 forwardanalyzer.cpp](https://github.com/danmar/cppcheck/blob/2.21.1/lib/forwardanalyzer.cpp)
  emits the notice before bailing from the limited traversal. It is not a
  diagnosed source defect and does not mean the remaining branches were checked.
- [Cppcheck 2.13 CLI](https://github.com/danmar/cppcheck/blob/2.13.0/cli/cmdlineparser.cpp)
  and [settings](https://github.com/danmar/cppcheck/blob/2.13.0/lib/settings.cpp)
  already support the exhaustive option, although their normal mode has no
  four-forward-branch notice. The control runs on older tools too: exhaustive
  completion and real-defect rejection remain mandatory, while the exact normal
  cutoff notice is required only on the verified 2.21 series. Newer versions
  may improve normal exploration without invalidating the exhaustive policy.
- [Ubuntu 24.04 package](https://packages.ubuntu.com/noble/cppcheck) identifies
  2.13.0-2ubuntu3. CI installs the distro package on `ubuntu-24.04` or the
  configured ARC runner. The ARC image's actual installed version must be read
  from its run; it is not inferred from the hosted-runner fallback.

The installed help and official sources were checked on 2026-09-08. Source
compatibility of the option is established for 2.13; a 2.13 runtime was not
executed in this local investigation.

## Full-profile acceptance measurement

The complete configured CPU profile at integration source
`3b65d0df3a132f7aace6cb1bef31449b44e32ad3` (tree
`78fef7ec3ff434dccfea39fedd4e218d5cfa6066`) analyzed all **1,177 commands across
281 sources** with Cppcheck 2.21.1. One serial analyzer process completed in
**93.31 seconds**, using **62,472 KiB peak RSS** measured directly with Linux
`wait4`. It ran on CPU affinity 16–19 in an isolated 8 GiB container, without
network or accelerator access; source, generated build and harness mounts were
read-only. The 30-minute failure timeout did not fire.

The result was **exit 1**, comprising 347 style diagnostics, three
`missingInclude` notices for quoted `stdio.h`, and the informational checker
inventory. No branch-budget notices or error/warning/performance/portability
diagnostics remained. The eleven native files changed by the latest integrated
parser/ADM/CAMBI/pool-consumer/PSNR chunk had no findings. This supports the
configuration's runtime feasibility, not a green whole-tree lint claim.

Before/after source/tree, tracked native files, clean Git status, analyzer and
POSIX-model hashes matched. The private compilation database remained identical
to the original. Raw XML, all findings grouped by primary source location,
arguments, child CPU/RSS/timing and identity receipts are retained under
`.workingdir2/evidence/cppcheck-exhaustive-analysis-20260908/`.
No same-source full normal-mode timing was repeated, so this measurement does
not establish a slowdown ratio. Other versions, hardware and backend profiles
still require their own execution; no time or memory allowance was increased.

## Reproducer and decision

```bash
python3 -m unittest discover -s scripts/ci/tests -p test_cppcheck_posix_model.py
python3 -m unittest discover -s scripts/ci/tests -p test_lint_configured.py
```

[ADR-1245](../adr/1245-cppcheck-exhaustive-configured-analysis.md) records the
runtime/coverage tradeoff. Neither diagnostic selections nor compilation
variants are reduced. Missing tools/models, parse errors, timeouts and analyzer
failures remain failures. This configuration is not a claim that all Cppcheck
analysis is mathematically exhaustive or that every backend has been configured.
