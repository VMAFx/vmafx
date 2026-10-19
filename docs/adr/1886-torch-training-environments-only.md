<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1886: torch only where training runs

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: Lusoris
- **Tags**: security, dependencies, ai, mcp, vmaf-tune, python, fork-local

## Context

The dependency dashboard (issue #941) reports nine PyTorch advisories
(PYSEC-2025-189, -190, -192 to -197, -210). OSV names no fixed release for
any of them, so no version bump clears them. Each one is reported once for
every package that declares torch:

- `ai/` (vmaf-train);
- `tools/ensemble-training-kit/`;
- `tools/vmaf-tune/`, through its `train` extra;
- `mcp-server/vmaf-mcp/`, through its `vlm` extra.

The last two are runtime packages.

- vmaf-tune needed torch only for `vmaftune.predictor_train`. That is the
  offline trainer of the predictor models. At run time vmaf-tune loads the
  trained models through ONNX Runtime.
- vmaf-mcp needed torch and transformers only for the descriptions of
  `describe_worst_frames`. On first use it downloaded SmolVLM, or Moondream2 as
  a fallback, from a model hub with `trust_remote_code=True`, which runs model
  code fetched at run time.

ONNX Runtime GenAI 0.17.1 (`onnxruntime-genai`, MIT, wheels for CPython 3.11
to 3.14, released 2026-09-29) runs vision-language models from a directory of
ONNX graphs. Its supported models include Phi-3.5-vision (published in ONNX by
its authors under MIT, 3.2 GB for the CPU build) and the Qwen vision models. We
measured it on 2026-10-05 with the CPU build of Phi-3.5-vision-instruct-onnx on
a frame of the Netflix 576x324 distorted clip, on four cores:

- the model loaded in 2.3 s;
- the description took 47.8 s, with an 8.6 GB peak resident set;
- the description itself: "The image shows signs of compression artifacts,
  particularly in the form of blocky areas and pixelation, especially
  noticeable in the foliage and the figures of the people, indicating a loss of
  detail and possible low bitrate encoding."

## Decision

torch stays only in the two training environments, `ai/` and
`tools/ensemble-training-kit/`.

- `vmaftune.predictor_train` moves into the training package as
  `vmaf_train.predictor_train`, with its tests. vmaf-tune keeps the runtime
  predictor and its ONNX models.
- `describe_worst_frames` describes frames through ONNX Runtime GenAI from a
  local model directory named by `VMAF_MCP_VLM_MODEL` (the `vlm` extra). It
  downloads nothing. Without the extra or the directory, it returns frame
  metadata with a note that says what is missing. A configured model that fails
  to load is an error.
- `scripts/ci/check-torch-scope.py` fails when any other package names a
  torch-family distribution in any dependency group, or when a lock inside
  another package resolves one.
- `security/vex/torch.openvex.json` records, for each advisory, why the training
  environments are not affected.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep torch in the optional extras and suppress the advisories | No code change | Runtime packages keep an install path to a library with unfixed advisories. The VLM path keeps executing hub code (`trust_remote_code`). A suppression records no reason. | The maintainer chose to confine torch to the training environments |
| Metadata-only `describe_worst_frames` (drop the VLM) | Smallest change, no new dependency | Loses a documented capability that a maintained ONNX path can provide | ONNX Runtime GenAI runs a maintained, MIT-licensed vision model; measured above |
| Hand-written generation loop on plain `onnxruntime` with exported SmolVLM ONNX graphs | No new package | Per-model vision encoder, embedding merge and KV-cache loop to maintain; no maintained Python API | ONNX Runtime GenAI already maintains that loop for several model families |
| Download the ONNX model on first use | Works without setup | Fetches gigabytes at tool-call time, with an integrity story to design | A local, operator-provided directory is explicit; the docs name a verified model |
| Keep `predictor_train` in vmaf-tune behind an import guard | No file move | The package would still declare torch, which is what the advisories are counted against | Moving the trainer removes the declaration |

## Consequences

- **Positive**:
  - vmaf-mcp, vmaf-tune and every other runtime package resolve no torch.
  - The MCP server no longer executes model-hub code.
  - The advisories are counted against two training packages, each with a
    recorded justification.
- **Negative**:
  - `pip install vmaf-mcp[vlm]` no longer gives descriptions on its own: an
    operator must also provide a model directory.
  - A description costs about 48 s per frame on four CPU cores and needs about
    9 GB of memory.
  - Predictor training now runs from the ai/ environment
    (`python -m vmaf_train.predictor_train`).
- **Neutral / follow-ups**:
  - The `vmaf-tune-train` test suite is gone: its tests run in the ai suite.
  - The OpenVEX statements must be revisited when a torch release fixes an
    advisory, or when training code starts to use an affected function.

## Supply-chain impact

- **New dependencies**: `onnxruntime-genai>=0.17.1` (optional `vlm` extra of
  vmaf-mcp, MIT, <https://github.com/microsoft/onnxruntime-genai>, runtime).
- **Removed dependencies**: `torch`, `transformers`, `accelerate` and `Pillow`
  from vmaf-mcp's `vlm` extra; `torch` from vmaf-tune's `train` extra (now
  removed). None of them was in a lock of those packages.

## References

- Maintainer decision relayed by the orchestrator on 2026-10-05 (paraphrased):
  keep torch only where training runs; move the MCP vision-language fallback to
  ONNX Runtime if a maintained ONNX path exists, otherwise fall back to
  metadata; move the predictor trainer into the ai/ package; add a contract
  that no runtime package declares torch; record OpenVEX statements for the
  training packages.
- Advisories: <https://osv.dev/vulnerability/PYSEC-2025-189> and the eight
  others named in `security/vex/torch.openvex.json`.
- ONNX Runtime GenAI 0.17.1: <https://pypi.org/project/onnxruntime-genai/0.17.1/>;
  supported models in its README; multimodal example
  `examples/python/model-mm.py` (v0.17.0).
- Phi-3.5-vision-instruct-onnx: <https://huggingface.co/microsoft/Phi-3.5-vision-instruct-onnx>
  (MIT).
- OpenVEX specification v0.2.0: <https://github.com/openvex/spec/blob/main/OPENVEX-SPEC.md>.
