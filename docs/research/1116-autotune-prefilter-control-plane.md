<!-- markdownlint-disable MD013 MD060 -->
# Research-1116: vmaf-tune autotune prefilter control plane (Pelorus deband)

- **Date**: 2026-06-14
- **Companion ADR**: [ADR-1116](../adr/1116-autotune-prefilter-control-plane.md)
- **Scope**: Integration-plan workstreams D1 (filter adapter) + D2
  (`vmaf-tune prefilter` joint TPE subcommand).
- **Status**: Snapshot at implementation time.

## Question

The Pelorus deband filter exposes a frozen 10-knob AVOption contract
(Pelorus ADR-0110). How should vmaf-tune drive a VMAF-in-the-loop search
over those strengths plus CRF, given that vmafx must stay Vulkan-free
(emit `-vf` strings, score the encoded output — never run the filter)?
Three sub-questions:

1. Where does a pre-encode filter fit in vmaf-tune's adapter taxonomy?
2. How do we co-optimise the deband knobs and CRF without reinventing
   the search engine?
3. How is the contract kept from silently drifting from the Pelorus
   side, and how is the live (filter-required) path gated?

## Findings

### 1. Filter ≠ codec → a new `filter_adapters/` family

vmaf-tune's `codec_adapters/` (ADR-0237) model a `-c:v` encoder: a
quality knob (CRF/CQ), presets, two-pass, GOP control. A pre-encode
filter has none of that surface — it has strength knobs and emits a
`-vf` fragment. Bolting a "filter" onto `CodecAdapter` would leak
filter-only fields into a Protocol every codec must satisfy. The clean
design is a **sibling family**, `filter_adapters/`, with a parallel
`FilterAdapter` Protocol. This mirrors the codec-adapter precedent: the
search loop consumes the declared knobs + the `vf_fragment` emitter and
never branches on the adapter's `name`, so a second pre-filter (sharpen,
denoise) is a one-file addition.

### 2. Joint TPE over deband knobs + CRF

The deband strength and CRF are **not separable**: debanding changes the
rate-distortion curve (it smooths gradients, which both raises perceived
quality and changes the bit cost at a given CRF). A nested search (CRF
bisect inside a deband sweep) ignores this coupling and costs
O(knobs × bisect) encodes. Instead, we lift `fast.py`'s single-axis
Optuna `TPESampler` objective to a **joint 11-dimensional search space**
— the 10 deband dims (generated straight from the adapter's frozen knob
table) plus a synthetic ordinal `crf` dim — and minimise
`|achieved_vmaf − target| + λ·kbps` in one objective call. TPE proposes
a full `(deband-dict, crf)` per trial; the probe runs
`deband → encode → score` and returns the achieved VMAF + bitrate. The
λ·kbps tie-breaker (1e-4, same weight as the fast path) steers ties
toward the lowest-bitrate combination that hits the target.

Knob-type handling follows the contract: `range`/`dither`/`dynamic`/
`protect` are suggested via `suggest_int` (ordinal, per the contract's
"`range` is an integer search dimension — treat as ordinal, not
continuous" note); `thry`/`thrc`/`grainy`/`grainc`/`softness`/`detail`
via `suggest_float` over their continuous ranges. Default trial budget
is 60 (vs. the fast path's 30) because the search space is wider; the
smoke surface and the injected-probe tests confirm convergence well
inside the soft time budget.

### 3. Contract drift detection + live-path gating

The adapter hard-codes the 10 knobs verbatim from ADR-0110, and a
**contract-conformance test** (`test_filter_adapter_pelorus_deband.py`)
re-transcribes the ADR-0110 table independently and asserts the adapter
matches it knob-by-knob (type / min / max / default). Any drift on the
vmafx side — or a stale copy after a Pelorus contract change — fails the
gate, which is the two-repo break detector the contract's "Changing the
contract" section calls for.

The live `deband → encode → score` loop requires the Pelorus Vulkan
filter compiled into the ffmpeg build. We gate it behind
`pelorus_filter_available()` (greps `ffmpeg -filters` for
`pelorus_deband_vulkan`); the CLI handler refuses the live run with an
actionable message when the filter is absent, and points the operator at
`--smoke`. This is the only Vulkan-adjacent coupling, and it is a
runtime probe — vmafx ships no Vulkan code.

## Validation posture

Pelorus and the deband ffmpeg filter are **not installed in this
environment**, so the live encode path cannot run here. Coverage is
unit-level and deterministic:

- **Adapter**: knob→`-vf` emission, contract-ordered output, range
  clamping, and validation (out-of-range, out-of-contract
  `sample`/`blur`/`planes`/`meta`, fractional-integral) — no ffmpeg.
- **Search**: search-space construction (10 knobs + CRF), the synthetic
  joint smoke search converging toward target, and an injected probe
  closing the loop without ffmpeg.
- **Subcommand**: argparse wiring, the filter-availability gate, and a
  fully-mocked encode/score loop asserting the deband `-vf` fragment is
  injected into every `EncodeRequest`.

The live path is designed against a real ffmpeg-with-Pelorus build and
should be smoke-tested there before any production use.

## References

- Pelorus ADR-0110 control-plane contract (`pelorus/docs/api/control-plane.md`).
- Pelorus ADR-0106 (autotune coupling modes).
- [ADR-0276](../adr/0276-vmaf-tune-fast-path.md) / [ADR-0304](../adr/0304-vmaf-tune-fast-path-prod-wiring.md) (the reused Optuna TPE study).
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) (codec-adapter family precedent).
- Integration plan: `.workingdir2/rc/pelorus/PLAN.md`.
