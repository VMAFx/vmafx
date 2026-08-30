<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1124: vmafx-tune-go Stage 5 — per-shot tuning, codec-adapter table, and backend resolution

- **Status**: Accepted
- **Date**: 2026-08-30
- **Deciders**: Lusoris
- **Tags**: `go`, `vmaf-tune`, `migration`, `codec-adapters`, `cli`

## Context

[ADR-0705](0705-vmafx-tune-go-stage1.md) stages the Go port of `vmaf-tune`: the
Go binary ships as `vmafx-tune-go` alongside the Python one, and "Stage 3 (swap)
will rename when feature parity is reached" — the same ship-alongside posture
[ADR-0703](0703-vmafx-server-go-grpc.md) and
[ADR-0704](0704-vmafx-mcp-go-port.md) take for the server and MCP ports. Stages
1–4 landed `compare`, `ladder` and `report`. Stage 5 is `tune-per-shot`, the
"Netflix per-shot encoding" feature ([ADR-0392](0392-vmaf-tune-phase-d-per-shot.md)
Phase D): cut the source into shots, bisect a CRF per shot, emit an FFmpeg
encoding plan.

Porting it surfaced three gaps the existing Go packages could not cover, each of
which admits more than one reasonable answer.

**Codec knowledge.** `pkg/encoder.Encoder` answers "run one encode" and exposes
only `Name()` + `CRFRange()`. The per-shot plan has to carry a *codec-correct*
argv slice per segment — `-crf` for x264/x265/AV1 software, `-cq` for NVENC,
`-global_quality` for QSV, `-rc cqp -qp_i N -qp_p N` for AMF, `-cpu-used N` for
libaom (HP-1 / [ADR-0297](0297-vmaf-tune-encode-multi-codec.md)) —
and has to clamp each recommendation into the codec's informative quality window,
which is *not* the window the bisect searches
([ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md)). The Python answers both
from its 17-adapter registry; Go had neither fact.

**Backend resolution.** `--score-backend` must fail fast on an unavailable
backend rather than silently downgrading ([ADR-0667](0667-vmaf-tune-score-backend-native-priority.md)).
`pkg/gpu.Detect()` exists but answers a different question: it returns the
*first* vendor found so a `vmafx-node` can advertise a primary backend. On a host
with both an NVIDIA and an Intel GPU it would never surface `sycl`, so an
explicit `--score-backend sycl` would be wrongly rejected.

**Two Python-only flags.** `--predicate-module MODULE:CALLABLE` imports a Python
callable at runtime. `--fast-nr` runs the `nr_metric_v1` ONNX model through
`onnxruntime` for NR early-elimination ([ADR-0624](0624-fast-nr-prescoring-impl.md)).
Neither has a Go implementation, and neither can be faked.

## Decision

We ship Stage 5 as `pkg/pershot` (shot ranges, uniform-window splitter, detector
parsers, the `PredicateFn` tuning seam, plan construction, and a
byte-compatible plan-JSON emitter) plus `pkg/scorebackend` (an independent
per-vendor probe of the four libvmaf backends, intersected with what
`vmaf --help` advertises).

`pkg/encoder` gains a codec-adapter policy table covering exactly the ten codecs
its `NewExtended` factory can construct, carrying both quality windows and the
per-codec argv shape, plus an `AdapterEncoder` that renders its ffmpeg argv
through that table. `--encoder` accepts those ten and rejects the other seven
Python adapters by name, pointing at the Python binary.

`--predicate-module` and `--fast-nr` are **registered and fail fast** with an
error naming the Python fallback, rather than being omitted or silently ignored.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Codec knowledge**: adapter table in `pkg/encoder` (chosen) | One home for codec identity, ranges and argv; reusable by the `fast` / `auto` ports; additive to `compare` / `ladder` | Grows a package that was deliberately thin | Chosen — the alternative duplicates codec facts in every consumer |
| **Codec knowledge**: unexported table inside `pkg/pershot` | Smallest blast radius | Next port re-derives the same 17-row table; two copies drift | Duplication is the exact failure the adapter contract exists to prevent |
| **Codec knowledge**: add `CodecArgs`/`Presets` methods to the `Encoder` interface | No new type | Widens an interface every encoder must satisfy, including future ones that have no preset vocabulary | Interface widening for a minority need |
| **Backends**: new `pkg/scorebackend` (chosen) | Probes each vendor independently, matching the Python; strict-mode failure is honest | A second GPU-probing package next to `pkg/gpu` | Chosen — the semantics genuinely differ, and the doc comment says so |
| **Backends**: reuse `pkg/gpu.Detect()` | No new package | Returns only the first vendor, so an explicit `--score-backend sycl` on a mixed-GPU host would be wrongly rejected | Produces incorrect refusals on exactly the dev machine this fork targets |
| **Unported flags**: register + fail fast (chosen) | CLI surface matches the Python; an operator learns immediately and is told where to go | A flag that never works is visible in `--help` | Chosen — the help text says NOT IMPLEMENTED and names the fallback |
| **Unported flags**: omit them | No dead surface | `--fast-nr` becomes "unknown flag", which reads as a typo rather than a scope gap | Worse diagnostics for the same outcome |
| **Unported flags**: accept and ignore | Scripts keep running | Silently changes semantics: a custom predicate is replaced by the bisect; NR elimination just does not happen | Silent semantic change is the worst option |
| **Plan JSON**: byte-compatible with Python (chosen) | Existing consumers and the `report` ingester read Go output unchanged; a diff harness proves parity | Requires reproducing `sort_keys`, `repr()` floats and `ensure_ascii` | Chosen — verified identical across all ten codecs |
| **Plan JSON**: schema-compatible only (the Stage 1–4 precedent) | Less code | A byte diff against the Python is then not a regression signal, only a curiosity | Cheap to do properly here, and it makes the parity claim testable |

## Consequences

- **Positive**: `tune-per-shot` is the fourth of seventeen subcommands with a
  real Go implementation. `pkg/encoder`'s adapter table and `pkg/scorebackend`
  are prerequisites the `fast`, `auto` and `corpus` ports will not have to
  re-derive. Plan JSON is byte-identical to the Python across every supported
  codec, so the ADR-0705 Stage-3 parity gate gains a verifiable check rather
  than an eyeball comparison.
- **Positive (incidental)**: adding `EncodeParams.InputArgs` fixed a live bug —
  the QSV device-init chain was being emitted after `-c:v`, where ffmpeg rejects
  it with `-22`, breaking every Go-driven QSV encode (`compare` and `ladder`
  included). See `docs/state.md` T-GO-QSV-INIT-CHAIN-PLACEMENT-2026-08-30.
- **Negative**: `--encoder` covers ten of the Python's seventeen codecs. The
  seven remaining (`av1_nvenc`, `av1_qsv`, `av1_amf`, the four VideoToolbox
  adapters, `libvvenc`, `libvpx-vp9`) need `pkg/encoder` implementations before
  the adapter table can grow to match.
- **Negative**: two flags exist that always fail. They are the honest
  representation of a scope gap, but they are dead surface until Stage 6.
- **Neutral / follow-ups**: `--fast-nr` is blocked on an ONNX Go binding, the
  same dependency the `fast` subcommand needs — the two should land together in
  Stage 6. `--predicate-module` has no Go equivalent by construction; the
  library seam `pershot.PredicateFn` covers Go callers, and the flag stays a
  permanent Python-only affordance unless a plugin mechanism is introduced.
  The Python is untouched: this work makes the sunset possible, it is not the
  sunset.

## References

- [ADR-0705](0705-vmafx-tune-go-stage1.md) — staged Go port, Stage roadmap.
- [ADR-0730](0730-vmafx-tune-go-stage2.md) — Stage 2 (`ladder`, hardware encoders).
- [ADR-0770](0770-vmafx-tune-go-stage4-report.md) — Stage 4 (`report`).
- [ADR-0703](0703-vmafx-server-go-grpc.md), [ADR-0704](0704-vmafx-mcp-go-port.md) — the Go binaries ship *alongside* their Python counterparts; the Python implementation is not removed.
- [ADR-0297](0297-vmaf-tune-encode-multi-codec.md) + [ADR-0399](0399-vmaftune-codec-adapter-runtime-contract.md) — codec-agnostic encode dispatch and the adapter runtime contract (HP-1).
- [ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md) — the bisect searches the encoder's absolute CRF range so premium-archival targets stay reachable.
- [ADR-0601](0601-vmaftune-qsv-amf-hw-init-and-probe-fix.md) — QSV pre-input device-init chain.
- [ADR-0299](0299-vmaf-tune-gpu-score.md) + [ADR-0667](0667-vmaf-tune-score-backend-native-priority.md) — `--score-backend`, native-first `auto` priority, and the strict explicit-request rule.
- [ADR-0513](0513-per-shot-scene-threshold-and-1-shot-chart.md) — `--scene-threshold` + `--max-shot-duration`.
- [ADR-0548](0548-vmaf-tune-full-file-and-no-bisect.md) + [ADR-0509](0509-vmaf-tune-compare-container-source-framerate-probe.md) — container sources accepted directly, geometry auto-probed via ffprobe.
- [ADR-0222](0222-vmaf-per-shot-tool.md), [ADR-0223](0223-transnet-v2-shot-detector.md) — the shot detector.
- Source: task brief for the Go-migration per-shot group (paraphrased: port
  `tune-per-shot` to Go reusing the existing `pkg/*` packages, and report a
  precise, evidence-backed blocker rather than producing a hollow stub where a
  faithful port is not achievable).
