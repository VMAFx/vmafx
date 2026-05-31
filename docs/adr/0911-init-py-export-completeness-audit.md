<!-- markdownlint-disable MD013 MD060 -->
# ADR-0911: `__init__.py` export-completeness audit — `__all__` + SPDX headers across fork-added Python packages

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude (audit run)
- **Tags**: `docs`, `python`, `ai`, `mcp`, `tools`, `lint`

## Context

A focused audit of every `__init__.py` in the fork-added Python tree
(`ai/`, `dev-llm/`, `mcp-server/`, `scripts/lib/`, `tools/*/src/`) found
eight package surfaces in three failure modes:

1. **Substantive docstring listing sub-modules, but no `__all__`** — the
   docstring is a non-machine-readable contract; static analyzers
   (pyright, ruff `F401`, IDE auto-import) cannot use it. Affected:
   `ai/__init__.py`, `ai/data/__init__.py`, `ai/train/__init__.py`,
   `scripts/lib/__init__.py`, `ai/src/vmaf_train/data/__init__.py`.
2. **Bare `__version__` with no SPDX header / no docstring** — fails
   the fork's per-file `Copyright 2026 Lusoris and Claude (Anthropic)` /
   SPDX-License-Identifier convention (CLAUDE.md §12 r7). Affected:
   `ai/src/vmaf_train/__init__.py`,
   `dev-llm/src/vmaf_dev_llm/__init__.py`,
   `mcp-server/vmaf-mcp/src/vmaf_mcp/__init__.py`.
3. **Stale docstring** — `ai/train/__init__.py` enumerated 3 sub-modules
   when 6 actually existed (`konvid_pair_dataset`, `qat`,
   `train_combined` added since the original write-up). Per the user-
   scope "fix pre-existing inaccuracies in files you touch" rule, the
   docstring is refreshed in the same change.

The well-formed examples already in tree
(`ai/src/aiutils/__init__.py`, `ai/src/corpus/__init__.py`,
`tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py`) established
the pattern; this ADR codifies it and brings the laggards into line.

Upstream-mirror packages (`compat/python-vmaf/**`,
`python/test/__init__.py`) and pure test-marker stubs
(`ai/tests/__init__.py`, `tools/external-bench/tests/__init__.py`) are
explicitly out of scope — the upstream files preserve Netflix headers
byte-identical for rebase hygiene, and empty test markers carry no
public-symbol claim worth advertising.

## Decision

Every fork-added Python package's `__init__.py` exposes:

1. **SPDX + Lusoris copyright header** (per CLAUDE.md §12 r7).
2. **Module docstring** that lists the package's sub-modules with one-
   line descriptions.
3. **`__all__`** as the canonical, machine-readable list of *what the
   package surface exports*. For namespace packages (the common case
   here — `ai`, `ai.data`, `vmaf_train.data`, etc.) `__all__` lists
   sub-module names; for packages that genuinely re-export symbols
   (e.g. `aiutils`, `corpus`), `__all__` lists the re-exported symbols.
   Packages whose only module-level surface is `__version__` declare
   `__all__ = ["__version__"]` so that fact is explicit.

Empty test-marker `__init__.py` files stay empty — they carry no claim
worth advertising and adding ceremony to them would be busywork.
Upstream-mirror files (Netflix copyright) are untouched.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Codify the pattern + fix the 8 laggards (chosen)** | One PR closes the gap; future packages have a written convention to follow; `__all__` becomes a machine-readable contract for static analysis | Adds boilerplate to small files | The convention already existed informally in the well-formed examples; codifying it makes the lint-clean baseline explicit |
| Add a `ruff` / custom lint rule that *requires* `__all__` in every package | Mechanical enforcement going forward | Ruff has no built-in for this; a custom rule would add a maintenance burden; would also fail upstream-mirror files unless excluded by path | Heavyweight for the size of the problem; the existing pre-commit `check-init-py` style lint we already run via `make lint` plus reviewer-side checking is sufficient |
| Skip the `__version__`-only packages — they're trivially small | Less diff churn | Misses the SPDX-header requirement (CLAUDE.md §12 r7) that applies regardless of file size; leaves the inconsistency for the next audit | Half-finished work that re-burns context on the next sweep |
| Leave the stale `ai/train/__init__.py` docstring (3 of 6 sub-modules) untouched | Smaller diff | Violates user-scope rule "fix pre-existing inaccuracies in files you touch"; future readers misled | The whole point of the audit is to leave the surface accurate |

## Consequences

- **Positive**: Pyright / IDE auto-import / `from pkg import *` now see
  the package surface accurately. `make lint` baseline is uniform
  across fork-added Python packages. SPDX coverage is 100 % on
  fork-added init files. Stale `ai/train/__init__.py` docstring no
  longer lies about the available sub-modules.
- **Negative**: Eight files grow by 5–15 lines each. Future package
  additions must remember the pattern (mitigation: this ADR is the
  reference; reviewers cite it).
- **Neutral / follow-ups**: When a new fork-added Python package
  lands, the PR author follows this ADR's pattern. No CI gate added —
  the pre-existing `check-copyright` script (run via `make lint`) and
  reviewer judgement cover enforcement.

## References

- Source: agent-dispatch brief 2026-05-31 — "Empty `__init__.py` audit —
  every Python package has one with right `__all__`?"
- Related rule: CLAUDE.md §12 r7 (SPDX header on every fork-added
  source file).
- Related rule: CLAUDE.md §12 r10 / ADR-0100 (per-surface doc-substance
  rule — `__init__.py` is the canonical package-surface doc).
- Well-formed examples in tree:
  [`ai/src/aiutils/__init__.py`](../../ai/src/aiutils/__init__.py),
  [`ai/src/corpus/__init__.py`](../../ai/src/corpus/__init__.py),
  [`tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py).
- Research digest:
  [`docs/research/init-py-export-audit-2026-05-31.md`](../research/init-py-export-audit-2026-05-31.md).
