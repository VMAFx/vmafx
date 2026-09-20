# Research-2074: HISS-21 replay-evidence gap

## Scope

Compare the governance claims shown on VMAFx `master` with the installed and
CI-pinned Praetor engines, then measure whether the claimed enforcement can be
replayed rather than inferred from a green audit.

## Findings

- The live README badge and canonical `AGENTS.md` ended at HISS-16. Current
  Praetor defines HISS-17 through HISS-21, including replayable enforcement
  evidence and platform neutrality.
- The manifest and lock `version: 1` fields are schema versions, not the HISS
  revision. They did not explain the stale HISS-16 label.
- `praetorctl hiss coverage --verify` failed on the unmodified tree with
  `no coverage catalog at .config/hiss/coverage.yaml`.
- The required standards workflow ran `compile-context --verify` and `audit`
  only. It could pass without executing the HISS-20 verifier.
- Praetor commit `846da5908d15b3cf5581ca6b0205cc644b249599`, already pinned by
  the workflow, contains the coverage verifier. No engine bump is required to
  close this gap.

## Evidence design

The catalog records only behavior the verifier replays itself. It covers six
scanner rules, 18 rule/language claims, and 43 fixtures across C, Go, Python,
and Rust. Each partial claim has at least one detecting fixture and, where the
scanner has a known boundary, a gap fixture. Legitimate negative fixtures guard
against overmatching.

Compiler warnings, ABI checks, supply-chain checks, context compilation, and
coverage floors are real VMAFx gates, but this verifier does not execute them.
Putting them in this catalog would prove attribution at most, not enforcement,
so they remain outside the replay claim.

## Required integration

The replay command must fail closed in four places: `make verify-all`, local
hooks, the required standards job, and a three-platform GitHub Actions matrix.
The required-check aggregator must name every matrix context and classify the
standards/replay contexts as strict-success checks. Naming alone is insufficient:
the generic aggregator policy accepts an absent check as a path-filter skip and
accepts `skipped` or `neutral` conclusions. A contract test therefore pins the
workflow matrix, local entrypoints, strict list, and success-only evaluator.

## Reproducer

```bash
praetorctl hiss coverage --verify
make hiss-coverage
```

Deleting the catalog, replacing a positive fixture with clean code, or turning
a negative fixture into a matching violation must make both commands nonzero.
