<!-- markdownlint-disable MD013 MD060 -->
# ADR-1278: Use spawn-safe Python process execution and the canonical five-parameter logistic curve

- **Status**: Proposed
- **Date**: 2026-09-21
- **Deciders**: VMAFx maintainers
- **Tags**: `python`, `testing`, `concurrency`, `numerical-correctness`, `dependencies`

## Context

The classic Python harness forced the POSIX `fork` start method at import time and
used fork-only global state to avoid pickling local functions. Python 3.14 warns
when a multithreaded process forks because the child inherits inconsistent thread
state. The same warning was emitted hundreds of times by the six-file regression
batch. Repeated assets also need mutual exclusion: two evaluations of the same
asset must never write the same workfiles concurrently, and result order must
remain input order.

Separately, the harness's five-parameter logistic (5PL) implementation added
`b1` instead of multiplying the logistic term by it. That made `b1` and `b5`
redundant, caused SciPy to report an unidentifiable covariance matrix, and used a
raw `exp()` expression that overflowed for large inputs. The Sheikh, Sabir, and
Bovik reference defines `b1` as the logistic amplitude.

## Decision

Use joblib's `loky` process backend for `parallel_map`, group equal asset keys
into serial units before dispatch, and reconstruct results in original input
order. Use an explicit `spawn` context for the remaining FIFO helper processes.
Add joblib as a direct Python runtime dependency. Implement the reference 5PL
equation with `scipy.special.expit`, so all five parameters are identifiable and
the sigmoid remains finite at extreme inputs. Configure every active root and
package-local pytest entry point to treat warnings as errors, remove the legacy
tox warning-plugin disable, and make pytest-asyncio's fixture-loop scope
explicit wherever the plugin is loaded. Close override-import temporary files
before reopening them and remove them in a `finally` block so resource lifetime
does not depend on garbage collection.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep raw `fork` and filter the warning | Small diff | Retains the unsafe operation and hides a runtime diagnostic | Warning suppression is not a fix and violates the whole-tree warning policy |
| Keep warnings advisory in pytest | Avoids immediate failures when dependencies change | Lets CI stay green while runtime and deprecation defects accumulate | Rejected; all warnings are errors and must be corrected at source |
| `ProcessPoolExecutor` with `spawn` or `forkserver` | Standard library only | Ordinary pickle cannot serialize the local functions and bound callables supported by the harness; shared locks also become context-sensitive | Breaks the existing callable contract and duplicate-asset execution |
| Thread workers | No process serialization or fork | CPU-heavy feature work would contend on the GIL and changes process-isolation semantics | Does not preserve the established execution model |
| `loky` plus keyed serial groups and explicit `spawn` | Cloudpickle supports local callables; no unsafe fork; preserves duplicate serialization and output order | Adds a direct dependency and process-start overhead | Chosen; it preserves behavior while removing the unsafe primitive |

## Consequences

- **Positive**: Python 3.14 no longer reports unsafe-fork warnings; local
  callables remain supported; duplicate assets cannot overlap; 5PL training no
  longer contains redundant parameters or overflow-prone sigmoid evaluation;
  override imports no longer leak temporary-file wrappers; future pytest
  warnings fail locally and in CI.
- **Negative**: worker startup is slower than `fork`, and joblib becomes an
  explicit runtime dependency rather than only a scikit-learn transitive
  dependency.
- **Neutral / follow-ups**: upstream syncs touching `tools/misc.py`,
  `core/executor.py`, or `core/train_test_model.py` must preserve these
  invariants and the warning-as-error regressions.

## Supply-chain impact

- **New dependency**: joblib `>=1.6.0`, runtime, BSD-3-Clause,
  <https://github.com/joblib/joblib>. It was already present transitively through
  scikit-learn; direct declaration makes the harness import truthful.
- **Build-time fetches**: no new fetch mechanism; normal Python package
  installation resolves the declared dependency.
- **CVE surface delta**: no native library, network listener, or privileged
  operation is added.

## SBOM delta

```yaml
# Components made direct (CycloneDX 1.5 fragment)
components:
  - type: library
    name: joblib
    version: ">=1.6.0"
    purl: pkg:pypi/joblib
    licenses:
      - license:
          id: BSD-3-Clause
```

## References

- [Python 3.14 multiprocessing contexts and start methods](https://docs.python.org/3/library/multiprocessing.html#contexts-and-start-methods)
- [joblib process-backend serialization](https://joblib.readthedocs.io/en/stable/auto_examples/serialization_and_wrappers.html)
- [SciPy `expit`](https://docs.scipy.org/doc/scipy/reference/generated/scipy.special.expit.html)
- [SciPy `curve_fit`](https://docs.scipy.org/doc/scipy/reference/generated/scipy.optimize.curve_fit.html)
- [Sheikh, Sabir, and Bovik, 2006, equation 3](https://utw10503.utweb.utexas.edu/publications/2006/hrs-transIP-06.pdf)
- [Research digest](../research/python-warning-root-causes-2026-09-21.md)
- Source: `req` — "no fucking warning or error is just ignored because of being og netflix code, fix them all ffs"
