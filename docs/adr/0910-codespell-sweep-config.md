# ADR-0910: Project-wide codespell config + sweep policy

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: docs, lint, tooling, fork-local

## Context

Source-tree typos accumulate silently across docs, ADRs, research notes,
changelog fragments, code comments, and even public-surface help strings
(e.g. `core/src/feature/cambi.c::tvi_threshold` shipping the help text
`"Visibilty threshold ..."`). No project-wide spell-check ran in CI; the
existing `make lint` chain (clang-tidy, cppcheck, ruff, semgrep,
include-what-you-use) catches none of this. The user asked for a one-shot
`codespell` sweep across source + docs + comments, with real typos fixed
and a project config committed so the sweep is repeatable on future PRs.

`codespell` is a single-binary, dictionary-based spell-checker with low
false-positive rate on English prose; it is the same tool the Netflix
upstream uses out-of-band for typo cleanups. Configuration lives in
either `.codespellrc` (ini) or `pyproject.toml`; the ini form is the
ecosystem standard and is the form pre-commit and CI configurations
expect.

The tricky part is **scope**. The repo is a fork on top of Netflix/vmaf
with three classes of files that codespell *will* flag but where
mass-fixing is the wrong move:

1. **Netflix-author / upstream-mirror sources** — `compat/python-vmaf/`,
   `python/test/`, `core/src/feature/{x86,arm64,cuda,hip,common,metal}`,
   `core/src/feature/{integer_adm.h,feature_extractor.h,cambi.c,
   tad_rust.c}`, `core/tools/{y4m_input.c,cli_parse.c,README.md}`,
   `core/src/svm.cpp`, `core/src/pdjson.c`, `core/README.md`,
   `core/test/test_picture.c`. These mirror upstream verbatim (or are
   vendored libraries); any fix re-appears as a conflict on the next
   `/sync-upstream`. Skip wholesale.
2. **Frozen ADRs** — per [`docs/adr/README.md`](README.md) "Immutable
   once Accepted", Accepted and Superseded ADR bodies are frozen.
   `## References` sections are additionally exempted in user-scope
   rules because they hold verbatim `req` / `Q<r>.<q>` citations. Most
   typo flags in `docs/adr/*.md` land in one of these two sections.
   Skip the affected ADR files individually.
3. **Verbatim quotes inside otherwise-fork-authored docs** — block
   quotes from third-party licenses (University of Waterloo IVC
   license uses "entirity"), `req`-cited user lines ("there are a lot
   of modules that arent even coded?"), and CHANGELOG / changelog-
   fragment legacy text that has already been rolled into a release.
   Ignore these words via `ignore-words-list` rather than excluding the
   file entirely so future content in the same file is still gated.

The remaining domain-specific false-positives are technical acronyms
(`ANE`, `HSA`, `SME`, `CANN`, `HSI`, `COO`, `ND`, `BU`), Linux device
node fragments (`renderD*`), SIMD/Go/Python variable names (`thi`,
`tlo`, `dout`, `iterm`, `disPath`, `fo`, `aks`), and valid English
hyphenations the project consistently uses (`re-use*`, `re-declare`,
`pre-emptive*`).

## Decision

Adopt a single `.codespellrc` at the repo root that:

- runs codespell across the whole tree by default,
- skips build outputs, binaries, large binary fixtures, lockfiles,
  models, and the Netflix-author / upstream-mirror / frozen-ADR files
  enumerated above,
- ignores the domain-specific acronyms, variable-name fragments,
  hyphenations, and verbatim-quote words enumerated above.

In the same PR, fix every remaining real typo the config surfaces. The
2026-05-30 sweep landed 3 fixes:

- `CONTRIBUTING.md:126` — `orginization` → `organization`
- `docs/metrics/cambi.md:15` — `brigher` → `brighter`
- `docs/metrics/cambi.md:58` — `Visibilty` → `Visibility`

Going forward, contributors are expected to run `codespell` locally
before pushing docs / research / changelog changes. CI integration is a
follow-up (a workflow row that runs `codespell --config .codespellrc`
on every PR). Until then the config is documentation: the next sweep
re-runs cleanly against tree state and only flags newly-introduced
typos.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Add codespell with `.codespellrc` and one-shot sweep (chosen) | Repeatable; documents skip / ignore rationale; surfaces future regressions on local re-run | Three small touch-ups of existing tree state | Best trade between coverage and footprint |
| Inline `pyproject.toml` `[tool.codespell]` section | Single config file for Python tooling | Mixes spell-check config with package metadata; weaker discoverability for non-Python contributors | Project lacks a root `pyproject.toml` and many contributors work in C/Go/docs |
| Skip the config, sweep only as a one-off | Smallest diff | Future drift unguarded; next agent re-discovers the same skip / ignore choices from scratch | Defeats the point of a sweep |
| Auto-fix everything codespell flags | Highest immediate hit rate | Breaks Netflix-mirror parity (upstream re-applies the typo on sync); rewrites verbatim license quotes; modifies frozen ADR bodies | Violates ADR-0024 / ADR-0106 / global rules |
| Switch to `typos-cli` (Rust) | Faster; richer config | Smaller ecosystem; no existing fork tooling depends on it | Codespell is the upstream norm; adopting a parallel tool would split the surface |

## Consequences

- **Positive**: future docs / research / changelog PRs catch typos
  before merge by running `codespell` locally; the `.codespellrc`
  encodes the project's "do not rewrite upstream / vendored / frozen"
  policy in a machine-readable form so future agents reuse it.
- **Negative**: the ignore-list grows as the project adds new
  acronyms / variable conventions; review burden on every PR that
  introduces new domain shorthand.
- **Neutral / follow-ups**: wire `codespell --config .codespellrc` into
  `make lint` and a GitHub Actions row once the ignore-list stabilises;
  re-run after every `/sync-upstream` to catch newly-imported typos in
  fork-authored files.

## References

- Source: `req` — Lusoris 2026-05-30: "Run `codespell` across the tree
  to find common spelling mistakes in source + docs + comments. Fix."
  (paraphrased)
- Codespell upstream: <https://github.com/codespell-project/codespell>
- Related: [ADR-0024](0024-netflix-golden-preserved.md) (Netflix-author
  files are read-only),
  [ADR-0100](0100-project-wide-doc-substance-rule.md) (doc-substance
  policy this complements), [ADR-0106](0106-adr-maintenance-rule.md)
  (ADR maintenance / immutability), [ADR-0141](0141-touched-file-cleanup-rule.md)
  (lint-clean touched files).
