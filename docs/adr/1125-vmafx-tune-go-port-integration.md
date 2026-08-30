<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1125: Reconciling seven independent vmafx-tune Go ports into one tree

- **Status**: Accepted
- **Date**: 2026-08-30
- **Deciders**: Lusoris
- **Tags**: `go`, `vmafx-tune`, `python-sunset`, `integration`, `fork-local`

## Context

The remaining fourteen `vmaf-tune` subcommands were ported to Go by seven agent
groups working in parallel, each in its own worktree branched from the same
commit. Every group's work is individually sound: all seven build, vet and pass
their own tests in isolation.

They were not, however, written against each other. Six packages were invented
independently by two or three groups at once, under the same import path:

| Package | Groups | Sizes (LOC) |
|---|---|---|
| `pkg/codecadapter` | 3, 4, 6 | 1822 / 877 / 1074 |
| `pkg/pershot` | 1, 6 | 1887 / 776 |
| `pkg/predictor` | 3, 6 | 484 / 2325 |
| `pkg/conformal` | 2, 6 | 1410 / 479 |
| `pkg/scorebackend` | 1, 2 | 516 / 770 |
| `internal/pyjson` | 4, 6 | 629 / 632 |

Some collisions are invisible to git. Groups 3 and 6 both defined `pkg/codecadapter`
but in differently-named files (`adapter.go` vs `codecadapter.go`), so the merge
reported no conflict and produced a package with two `Adapter` types. Only the
compiler surfaced it.

Six of the seven groups also edit the same three accumulator files —
`cmd/vmafx-tune/cmd/root.go`, its test, and `docs/usage/vmafx-tune-go.md` — each
registering its own subcommands and shortening the "not yet ported" list.

## Decision

Integrate onto one branch by merging group branches sequentially, resolving each
collision against evidence rather than by merge order, and letting `go build` /
`go test` gate every step.

**One implementation per package, chosen on evidence — not on size:**

| Package | Kept | Why |
|---|---|---|
| `pkg/codecadapter` | group 6 | Both registries register the **identical 19 codecs**, verified by enumerating `Known()`, so the interface-vs-struct choice costs no coverage. Group 3's per-codec method interface converts mechanically to group 6's struct fields. **This tiebreaker was not sufficient — see Consequences.** |
| `pkg/pershot` | group 1 | Superset; carries the byte-identical `plan_json` emitter verified against CPython across all ten supported codecs. |
| `pkg/predictor` | group 6 | Superset (adds `features.go`). Group 3's `Clamp` helper was carried across. |
| `pkg/conformal` | group 2 | Superset: adds `CVPlusCalibration`, the `Calibration` interface, load/save and `StaleCalibrationError`. |
| `pkg/scorebackend` | group 2 | Superset. |

**JSON encoding is the messiest case, and this ADR originally described it
wrong.** It claimed there were two CPython-JSON implementations, kept
deliberately. There are **four**, at import paths that never collided in git,
totalling ~2,641 lines:

| Package | LOC | Author | Consumers |
|---|---|---|---|
| `pkg/pyjson` | 723 | group 3 | `pkg/corpus/{corpus,encode,score,jsonl}.go` |
| `internal/pyjson` | 632 | group 6 | four `cmd/` files, `pkg/corpusrow` |
| `internal/pyjsonstrict` | 641 | group 4 | `pkg/benchmark`, `pkg/encodeprofile`, `cmd/encodeprofile` |
| `pkg/tune/pyjson` | 645 | group 5 | `cmd/sidecar`, `pkg/tune/{auto,executor,sidecar}` |

They are redundant rather than divergent, which was measured rather than
assumed: 200,000 random finite `float64` (arbitrary bit patterns, the 1e16 and
1e-4 exponent thresholds, subnormals, `-0.0`) produced **zero** disagreements
across all four, and 10,000 sampled renderings matched CPython's `repr()` and
`json.dumps()` exactly. Consolidation is therefore behaviour-preserving, and is
left as follow-up rather than bolted onto an already-large change.

**Where APIs disagreed, the receiving package grew the missing seam** rather
than the consumer being rewritten around it — `WithAlpha` and `IntervalFor` on
`conformal.SplitCalibration`, package-level `ResolveCodecArgs` / `DefaultPreset`
/ `LegacyCodecArgs` on `codecadapter`, `Clamp` on `predictor`. Each carries a
comment naming the merge as its origin.

Two behavioural details had to be preserved explicitly:

- The **package-level** `codecadapter.ResolveCodecArgs` validates the preset
  before building argv; the `(*Adapter)` method deliberately does not. Group 4's
  contract (and its test) treats an out-of-vocabulary preset as an error, while
  group 6's method is the low-level token builder. Both are now true.
- `exitCodeError` existed twice with identical fields but different receiver
  kinds. It is unified on the value receiver and gained `ExitCode()`, so both
  `exitCodeError{...}` and `&exitCodeError{...}` satisfy `exitCoder`, and
  `exitCodeOf` composes as the final fallback after the interface and
  `fastExitCode` checks. No group's exit contract was dropped.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Seven separate PRs, one per group | Small, independently reviewable | The duplicate packages collide no matter the order; whoever merges second does this same reconciliation without the other six branches in hand, six times over | Rejected — moves the work later and does it worse |
| Keep the largest implementation of each package | Trivial rule, no judgement | Size is not correctness: group 3's `codecadapter` is the largest and has the least Python-parity evidence | Rejected — the tiebreaker has to be evidence |
| Merge by branch order and let last-writer-win | Zero decisions | Silently drops capability; the group-3/group-6 collision produced no git conflict at all, so "last writer" would not even have been visible | Rejected outright |
| Namespace every group's package (`codecadapter3`, `codecadapter6`) | No reconciliation needed | Ships two registries of the same 19 codecs that will drift apart | Rejected — duplication with a deadline |
| Collapse both pyjson encoders into one | One JSON package | The two mirror different Python functions with different non-finite handling; one package would answer to two contracts | Rejected for pyjson specifically, accepted everywhere else |

## Consequences

**The codecadapter tiebreaker verified the wrong property.** Comparing the two
registries' codec *names* showed them identical and was taken as evidence of
equivalence. It is not: the kept group-6 registry had `InvertQuality` set to
`true` for all four VideoToolbox codecs, where the Python adapters set
`invert_quality=False` (a VideoToolbox `-q:v` is higher-value-is-higher-quality,
unlike every CRF/CQ/QP codec) — and the deleted group-3 registry had it right.
Two argv divergences survived the same way: ProRes tier 5 emitted `4444xq`
instead of `xq`, and libx265 fell back to ffmpeg's generic `-pass/-passlogfile`
instead of `-x265-params`. All three are fixed, and
`pkg/codecadapter/python_argv_parity_test.go` now pins the emitted argv for
every (codec, preset, quality) triple against the Python adapters, which is the
comparison that should have decided this in the first place.

**Positive.** All fourteen subcommands land together, with one implementation of
each shared package and the `vmaf-tune` Python CLI fully shadowed —
`root.go` no longer registers a single redirect stub, and the stub machinery is
deleted. `TestStubSubcommands` (which asserted stubs still existed) is inverted
into `TestNoStubSubcommandsRemain`, so re-introducing a stub now fails a test.

**Negative.** This is a 228-file, ~112k-insertion change. It is large because
the reconciliation is only correct when done with every branch present; splitting
it would mean performing the same merge repeatedly against partial information.

**Integration surfaced three latent defects** that each group's own suite could
not see:

1. `.gitignore` carried a bare `corpus.jsonl` pattern (intended for Phase A
   scratch output) that matches at any depth, so
   `pkg/benchmark/testdata/corpus.jsonl` was silently excluded from its own
   commit. Fixed with a `!**/testdata/corpus.jsonl` negation.
2. `.gitattributes` `* text=auto` normalised the CRLF out of the benchmark CSV
   golden fixtures. Python's `csv` module writes CRLF, so the committed goldens
   stopped matching the renderer — invisible in the authoring worktree, which
   still held the pre-normalisation bytes, and red on any fresh checkout. Fixed
   by exempting `pkg/benchmark/testdata/*.csv` from normalisation.
3. `pkg/libvmaf`'s cgo `LDFLAGS` names `-L${SRCDIR}/../../core/build-cpu/src`.
   When that directory does not exist — any checkout that has not built the C
   library — the linker does not fail; it silently falls through to a
   distro-installed `libvmaf`. On this workstation that is upstream 3.2.0 with
   **zero** `vmaf_dnn_*` symbols, so a Go binary can link against a library that
   is not this fork at all. Recorded here; the fix (failing closed when the
   fork's libvmaf is absent) is deliberately left out of this PR's scope.

## References

- `req` — user direction 2026-08-30: continue the Go migration and get local-only
  work to remote so the repo "gets cleaner not worse".
- [ADR-1124](1124-vmafx-tune-go-stage5-per-shot.md) — the group-1 per-shot port
  this integrates.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — changelog/ADR fragment
  pattern the seven groups' fragments follow.
- `docs/research/vmafx-tune-go-fast-2026-08-30.md` — the group-2 research digest.
