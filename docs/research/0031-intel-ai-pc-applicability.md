# Research-0031: Intel AI-PC NPU + EP applicability to tiny-AI / `dnn/`

- **Status**: Active
- **Workstream**: backlog T7-9 (no ADR yet — verdict is *defer*)
- **Last updated**: 2026-04-29

## Question

Should the fork's tiny-AI surface (`core/src/dnn/`, `--tiny-device`,
`docs/ai/inference.md`) add first-class support for Intel's "AI PC"
silicon — meaning the NPU (Neural Processing Unit) that ships on
Meteor Lake / Lunar Lake / Arrow Lake client CPUs, plus the integrated
Xe / Xe2 GPU on the same package — and if so, via which ONNX Runtime
execution provider?

Specifically:

1. What is the NPU + integrated AI-accelerator surface on Intel
   AI-PC platforms?
2. Which ORT execution providers (`OpenVINOExecutionProvider`,
   `DmlExecutionProvider` / DirectML, a future dedicated NPU EP)
   would have to land in [`docs/ai/inference.md`](../ai/inference.md)
   to expose them to `--tiny-device`?
3. What is the per-model fp16 / int8 quantization story for the
   AI-PC NPU?
4. What changes to the `--tiny-device` device-selector grammar fall
   out (`--tiny-device npu`, `--tiny-device dml`, ordering in
   `--tiny-device auto`)?
5. Recommend or defer based on shipping % of AI-PC silicon and
   overlap with the Arc A380 path the maintainer already runs?

## Sources

The Intel developer overview page
(<https://www.intel.com/content/www/us/en/developer/topic-technology/ai-pc/overview.html>)
was unreachable from this session's WebFetch sandbox; this digest
is therefore assembled from the assistant's training-context
knowledge plus the in-tree fork docs that already reference Intel
silicon. **All public-vendor claims below are flagged as
training-context summaries**, not freshly fetched citations, and
should be re-verified before any code lands. The in-tree sources
*are* fresh:

- [`docs/ai/inference.md`](../ai/inference.md) — the `--tiny-device`
  EP matrix (CPU / CUDA / OpenVINO / ROCm).
- [`docs/ai/quant-eps.md`](../ai/quant-eps.md) — fork measurements
  on the Intel Arc A380 via the OpenVINO 2026.1 GPU plugin.
- [`docs/research/0006-tinyai-ptq-accuracy-targets.md`](0006-tinyai-ptq-accuracy-targets.md)
  — int8 PTQ accuracy targets and the OpenVINO Arc int8 failure mode.
- [`core/src/dnn/ort_backend.c`](../../core/src/dnn/ort_backend.c)
  header comment — enumerates the EPs the fork already wires
  (`CPU / CUDA / OpenVINO / ROCm`).
- ORT execution-provider documentation
  (<https://onnxruntime.ai/docs/execution-providers/>) — canonical
  list of EPs Microsoft maintains; cited by name where the
  training-context summary needs anchoring.

## Findings

### 1. NPU + integrated AI accelerator surface on Intel AI-PC

*Training-context summary, not a freshly-fetched citation.* Intel's
"AI PC" marketing umbrella covers three on-package compute domains
that can run an ONNX graph:

- **CPU cores** with AVX-VNNI / AMX (already covered by ORT's
  CPUExecutionProvider; nothing new for the fork).
- **Integrated Xe / Xe2 GPU** (e.g. Meteor Lake's Arc Graphics,
  Lunar Lake's Xe2). Same programming surface as the discrete Arc
  A380 the maintainer runs in production: oneAPI / Level Zero /
  OpenVINO `GPU.0` device. *No new EP plumbing — the existing
  OpenVINO EP wiring covers it.*
- **NPU** — the dedicated low-power inference accelerator. Meteor
  Lake shipped a "NPU 3720" (~11 TOPS int8); Lunar Lake's "NPU 4"
  jumps to ~48 TOPS int8 to clear the Microsoft "Copilot+ PC" 40
  TOPS bar; Arrow Lake's desktop variant ships a smaller NPU. The
  NPU is exposed as an OpenVINO device named `NPU` (not `GPU.0`).

The first two domains are *already reachable* through the fork's
existing `--tiny-device openvino` path; only the NPU is genuinely
new surface.

### 2. ORT execution providers we would need

Three candidate EPs cover the AI-PC NPU on ONNX Runtime today
(per <https://onnxruntime.ai/docs/execution-providers/>):

| Candidate EP | Vendor | NPU coverage | Maintenance status |
| --- | --- | --- | --- |
| **OpenVINOExecutionProvider** with `device_type=NPU` | Intel | Yes, via OpenVINO's NPU plugin | Already linked into the fork; one-line config change |
| **DmlExecutionProvider (DirectML)** | Microsoft | Yes, via the DirectML NPU adapter on Windows ≥ 11 24H2 | Windows-only; not in fork's Linux-first build matrix |
| **Future "VitisAI"-style NPU EP** | Microsoft / Intel | Speculative; not GA at time of writing | Out of scope |

For Linux + Windows parity the OpenVINO EP is the only realistic
path: it already builds on both, the fork already links it on
Linux, and switching `device_type` from `GPU` to `NPU` is a sidecar
JSON / runtime-flag change rather than a new EP integration. The
DirectML path would buy Windows-native NPU coverage for users who
don't want OpenVINO at all, but that is a *separate, larger* PR
(Windows CI runner + DML EP build + new `--tiny-device dml`
keyword) and out of scope for this digest's verdict.

If we pursue the OpenVINO-NPU route, [`docs/ai/inference.md`](../ai/inference.md)
gains one row in the EP matrix:

```text
| `--tiny-device openvino-npu` | OpenVINOExecutionProvider, device_type=NPU |
  Intel AI-PC NPU only; falls back to CPU EP if no NPU present.
```

### 3. fp16 / int8 quantization story for the AI-PC NPU

*Training-context summary.* Intel's NPU is fundamentally an int8
inference engine with bf16 / fp16 support on the newer NPU 4
generation; fp32 is *not* a first-class precision. This is the
inverse of the current fork posture, where fp32 is the default and
fp16 / int8 are opt-in via [`--tiny-fp16`](../ai/inference.md) and
the PTQ pipeline in [Research-0006](0006-tinyai-ptq-accuracy-targets.md).

Concretely, to ship a tiny model that runs well on an AI-PC NPU we
would need:

- An **int8-PTQ-quantized variant** of every shipped tiny model, with
  validated accuracy gates per [Research-0006](0006-tinyai-ptq-accuracy-targets.md).
  We have these as of 2026-04-28 for the CUDA + OpenVINO-CPU paths.
- The known **int8 failure mode on Arc A380** documented in
  [`docs/ai/quant-eps.md`](../ai/quant-eps.md) §Headline findings
  (Conv int8 graphs fail to compile; MLP int8 graphs emit
  `inf`/`NaN`) is a *separate* OpenVINO `intel_gpu` plugin bug.
  The NPU plugin is a different code path; we cannot assume the
  Arc int8 result predicts NPU int8 behaviour either way without
  an actual measurement on Meteor Lake or Lunar Lake hardware.
- An **fp16-only path** as the safe fallback (NPU 4 supports fp16
  natively; NPU 3720 supports it with a perf hit). This already
  works through `--tiny-fp16` and would not need new wiring.

The quantization story therefore *blocks* on hardware access:
without a Meteor / Lunar / Arrow Lake test machine in CI we cannot
gate either the int8 path or the fp16-NPU path. **Verdict:
defer** — see §5.

### 4. `--tiny-device` device-selector implications

If we did pursue this, the cleanest grammar change is to keep
`--tiny-device openvino` meaning "OpenVINO EP, GPU device type
preferred, CPU fallback" (its current behaviour) and *add* a new
keyword that disambiguates the NPU device type:

```text
--tiny-device openvino       # GPU.0 → CPU fallback (today's behaviour)
--tiny-device openvino-npu   # NPU only, hard error if no NPU present
--tiny-device openvino-cpu   # CPU device type, skip GPU.0 probe
```

`--tiny-device auto` would *not* add NPU to the try-chain by default
— NPU has surprising performance characteristics on small graphs
(per training-context understanding: latency floor dominated by
power-state transitions for sub-millisecond inferences). Users who
want NPU pay the explicit `--tiny-device openvino-npu` opt-in.

`vmaf_dnn_session_attached_ep()` would gain two new return strings:
`"OpenVINO:NPU"` and (if-and-only-if we add the DirectML path
later) `"DirectML"`. Adding strings is API-compatible; consumers
already documented as "assert on the returned string" in
[`docs/ai/inference.md`](../ai/inference.md) §Graceful EP fallback.

This is roughly **20 lines of C** in `ort_backend.c` plus a `--help`
string update — small, but only worth landing if there is a
maintainer with the hardware to test it.

### 5. Recommend or defer? — shipping volume vs maintainer overlap

*Training-context summary on shipping volume.* Intel's AI-PC
silicon began shipping in late-2024 (Meteor Lake) and ramped
through 2025 (Lunar Lake / Arrow Lake). Public Intel investor
commentary in mid-2025 cited "tens of millions" of AI-PC units
shipped, with full-year 2026 guidance in the ~100M-unit range. By
end-2026 the *installed base* of Windows laptops with an Intel
NPU is therefore non-trivial — but the fork's current users are
predominantly developers running discrete-GPU Linux workstations
(CUDA + Arc A380 + ROCm), not laptop users running ad-hoc tiny-AI
inference. The volume is real; the *overlap with our user base*
is small.

**Critical maintainer-overlap fact**: the user already runs an Intel
Arc A380 in their primary dev workstation, exposed through the
existing **SYCL backend** (for libvmaf feature kernels) and
**OpenVINO EP** (for tiny-AI inference). The Arc A380 covers the
"Intel iGPU surface" of an AI-PC platform exactly — it shares the
oneAPI / Level Zero / OpenVINO GPU stack that an integrated Xe /
Xe2 GPU also exposes. So:

- **Integrated Xe GPU on AI-PC platforms is already supported**
  through `--tiny-device openvino`. No new code needed; users with
  an Intel iGPU laptop benefit from the existing path today. This
  is the answer to the "does Arc A380 obviate AI-PC support?"
  question: **for the iGPU portion, yes; for the NPU portion, no.**
- **NPU is genuinely new surface.** It is a different OpenVINO
  device (`NPU` vs `GPU.0`), it has int8-first precision policy,
  and the Arc A380 owner cannot test it (Arc dGPUs do not expose an
  NPU device). Adding NPU support without hardware to validate it
  would mean shipping an unverified path — exactly the failure mode
  the int8-on-Arc-A380 result warns against.

**Verdict: DEFER** the AI-PC NPU integration with the following
explicit re-evaluation triggers:

1. A maintainer or contributor acquires a Meteor / Lunar / Arrow
   Lake workstation or laptop and offers to run the int8 + fp16
   measurement matrix from [Research-0006](0006-tinyai-ptq-accuracy-targets.md)
   against `device_type=NPU`.
2. A user explicitly requests `--tiny-device openvino-npu` for a
   shipping product.
3. ONNX Runtime ships a dedicated NPU EP (rather than NPU-via-OpenVINO)
   that materially changes the integration cost.

Until any of those fire, the fork's existing `--tiny-device openvino`
path covers the *iGPU* portion of an AI-PC platform for free, and
the NPU portion is documented here so the next session does not
re-investigate from scratch.

## Alternatives explored

- **Pre-emptively wire `--tiny-device openvino-npu` even without
  hardware to test it**: rejected. Shipping an EP path with no CI
  coverage and no measured accuracy gate would risk silently broken
  inference for the first user who tries it; this is the same class
  of failure as the Arc int8 bug.
- **Add the DirectML EP for Windows-native NPU support**: rejected
  for this digest's scope — DirectML brings a Windows CI matrix the
  fork does not have today, and the OpenVINO-NPU path covers Linux
  Windows once hardware access is available. Re-evaluate if a
  Windows-specific user need surfaces.
- **Treat AI-PC NPU as out of scope permanently**: rejected.
  Volume is real and growing; the deferral is for "no
  hardware to validate", not "irrelevant audience".

## Open questions

- What is the actual NPU plugin maturity in OpenVINO 2026.1+ for
  the small-MLP shapes the fork ships? (Cannot answer without
  hardware.)
- Does the NPU plugin share the `intel_gpu` plugin's int8 layout
  bug, or does it have its own quantisation pipeline? (Cannot
  answer without hardware.)
- Would the fork's existing PTQ pipeline ([Research-0006](0006-tinyai-ptq-accuracy-targets.md))
  produce a graph the NPU plugin can compile, or does NPU need
  bespoke calibration?

## Related

- [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance bar this digest is shaped to.
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — the
  six-deliverable rule that mandated this digest.
- [Research-0006](0006-tinyai-ptq-accuracy-targets.md) — int8 PTQ
  pipeline this digest defers a NPU-specific re-run of.
- [`docs/ai/inference.md`](../ai/inference.md) — the EP matrix this
  digest declines to extend, *for now*.
- [`docs/ai/quant-eps.md`](../ai/quant-eps.md) — Arc A380 +
  OpenVINO-CPU quantisation results that anchor the NPU
  expectation.
- Backlog row T7-9 (`.workingdir2/BACKLOG.md`) — the workstream
  this closes.
