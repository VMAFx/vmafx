# ADR-1168: The default VMAF model is defined in exactly one place

- **Status:** Accepted
- **Date:** 2026-09-03
- **Deciders:** Lusoris
- **Supersedes:** none
- **Superseded by:** none

## Context

The fork picks a model for the user whenever the user names none. Before this
change, thirty-two separate places in the tree each spelled `"vmaf_v0.6.1"` as
their own answer to that question, across three languages:

| Component | Sites | Kind |
| --- | --- | --- |
| `core/tools/cli_parse.cpp` | 1 | the CLI's `--model`-absent fallback |
| `core/tools/vmaf_vpl.c` | 1 | the VPL tool's fallback |
| `core/src/mcp/compute_vmaf.c` | 1 | the C MCP server's request default |
| `pkg/libvmaf`, `pkg/scorecli`, `pkg/tune/executor`, `pkg/fast`, `pkg/bisect`, `pkg/corpus` | 8 | Go fallbacks and exported constants |
| `cmd/vmafx-tune` | 4 | Go flag defaults |
| `tools/vmaf-tune` | 16 | Python signature defaults, argparse defaults, and one `getattr` fallback |
| `tools/vmaf-roi-score` | 2 | Python signature and argparse defaults |

Nothing tied those together. Changing the fork's default meant finding all
thirty-two by hand and missing some — and a missed one is invisible, because a
component that quietly scores with a different model than its neighbours
produces plausible numbers, not an error.

The maintainer asked for this directly while reviewing the v1.0.16 model port:
per user direction, the default should be settable in one place rather than
hardcoded across the tree.

**A constraint discovered while implementing this.** Flipping the default from
`vmaf_v0.6.1` to `vmaf_v1.0.16_3d0h` was measured against the Netflix golden
gate. Exactly one assertion fails:
`vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`, which
passes `use_default_built_in_model: True` and then asserts the resulting VMAF
and feature scores. It exists precisely to pin the default model's numbers, so
any change of default necessarily breaks it, and
[ADR-0024](0024-netflix-golden-preserved.md) forbids editing it. Measured on
the standard 576x324 pair, the default score moves from 76.667831 to 82.816059.

This ADR therefore covers the mechanism only. The constant keeps its current
value, this change is behaviour-identical (271 golden tests pass, 0 fail), and
choosing the value is left to a separate decision that has to resolve the
conflict with ADR-0024 explicitly.

## Decision

`VMAF_DEFAULT_MODEL_VERSION` in `core/include/libvmaf/model.h` is the single
authoritative definition of the fork's default model.

1. **C and C++ that link libvmaf** use the macro directly.
2. **Other languages** call the new public accessor
   `vmaf_default_model_version()` where they already link the library, and
   otherwise carry a *mirror* constant: `pkg/model.DefaultVersion` for Go,
   `vmaftune.defaultmodel.DEFAULT_MODEL` and
   `vmafroiscore.defaultmodel.DEFAULT_MODEL` for the two Python tools. Mirrors
   exist because most of the Go tree and both Python tools deliberately do not
   link libvmaf; forcing cgo or a C extension on them purely to learn a string
   would make them unbuildable without the C library.
3. **The mirrors are kept honest mechanically, not by discipline.**
   `scripts/ci/check-default-model-single-source.sh` parses the header, fails if
   any mirror disagrees with it, and fails if any component reintroduces its own
   hardcoded default. It runs in `make lint` and as a pre-commit hook.
4. **Deliberate pins carry their reason in the source.** A line that must name a
   specific model regardless of the fork's default marks itself with a
   `vmaf-model-pin: <reason>` comment. The AOM CTC preset uses this: the CTC
   specification mandates `vmaf_v0.6.1` exactly, so it must *not* follow the
   fork's default.

Changing the fork's default model is now a one-line edit to the header, plus
whatever the gate reports.

## Consequences

- **Positive**: the default is one line. The gate makes a missed site a build
  failure instead of a silent behaviour difference, and it is tested in both
  directions by `scripts/ci/tests/test-default-model-single-source.sh`
  (twenty-two cases, covering mirror drift in Go and Python, every fallback
  spelling below, a deliberate pin, prose in a doc-comment and in a Python
  docstring, and a deleted authoritative macro).
- **Negative, and the reason the test matters**: the gate matches enumerated
  syntactic forms, not the bare literal, because 34 of the literal's 35
  occurrences are doc-comments and model-name lookup tables. Enumeration can
  miss a spelling, and three rounds of adversarial review each found one it had
  missed: `getattr(args, attr, "vmaf_v0.6.1")` shipping in `vmaftune/cli.py`;
  then a comment heuristic that classified `const char *model = "..."` as a
  comment and blinded the gate entirely; then `pop`, `setdefault`, `strdup`,
  `value_or`, ternaries and single-quoted literals. The heuristic is gone —
  every pattern now requires real assignment, return or call syntax around the
  literal, which prose never has — and each spelling is pinned by a test. A
  genuinely new spelling still needs adding to both the gate and its test.
- **Positive**: `vmaf_default_model_version()` lets a binding read the default
  from the library it is actually linked against, rather than from a string
  copied into another source tree years earlier.
- **Negative**: three mirror constants still exist. They are not a single
  textual definition; they are a single *authoritative* definition plus copies
  that cannot drift. Removing them entirely would mean generating them at build
  time, which buys little over a gate that already fails on drift and costs a
  build-step dependency in two Python packages that currently need none.
- **Negative**: the gate needs its allowlist maintained. A genuinely new
  place that must pin a model has to either carry the marker or be added to the
  path list, with a reason.
- **Neutral / follow-ups**: the value itself is unchanged, and switching it to
  `vmaf_v1.0.16_3d0h` remains open. It requires resolving the ADR-0024 conflict
  described above, and is now a one-line change once that decision is made.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **Header macro + accessor + gated mirrors (chosen)** | One authoritative definition; drift is a build failure; no new build-time dependency; pure-Go and pure-Python components stay buildable without the C library | Three mirror constants remain in the tree | Only option that is enforceable without forcing cgo on packages that deliberately avoid it |
| Generate the mirrors at build time from the header | Literally one textual definition | Adds a codegen step to two Python distributions and the Go build that neither needs today; generated files either get committed (and drift anyway) or break `go build` on a fresh checkout | Rejected — more machinery than the gate, same guarantee |
| Make everything link libvmaf and call `vmaf_default_model_version()` | No mirrors at all | Forces cgo on `vmafx-tune`, `pkg/fast`, `pkg/bisect`, `pkg/corpus` and a C extension on two Python tools, so none of them build without the C library present | Rejected — a large portability cost to avoid copying one string |
| A shared data file (JSON/text) read at runtime by every component | Language-neutral | Turns a compile-time constant into a runtime file lookup with a new failure mode (file missing or stale relative to the binary); the C library would have to read a file to answer "what is my default" | Rejected — trades a compile-time guarantee for a runtime one |
| Convention only: document the header as authoritative, no gate | Zero machinery | Exactly the status quo that produced thirty-two copies; the failure is silent | Rejected |

## References

- req — the maintainer, reviewing the v1.0.16 model port, asked that the default
  be settable in one place instead of being hardcoded across the tree
  (paraphrased: "perhaps use this so it is not hardcoded in 100 places but only
  one").
- [ADR-0024](0024-netflix-golden-preserved.md) — the Netflix golden assertions
  are the fork's numerical ground truth and are never edited. This is what makes
  the value change a separate decision from the mechanism.
- [ADR-1122](1122-vmaf-v1-model-port.md) — the v1.0.16 model port that made a
  newer default possible.
- [`docs/models/v1.md`](../models/v1.md) — identifies `vmaf_v1.0.16_3d0h` as the
  standard 1080p / 3H model, the direct counterpart to `vmaf_v0.6.1`.
- Measured on this branch: default score on the 576x324 golden pair moves from
  76.667831 (`vmaf_v0.6.1`) to 82.816059 (`vmaf_v1.0.16_3d0h`); with the value
  left unchanged, 271 golden tests pass and 0 fail.
