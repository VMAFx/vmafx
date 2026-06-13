# ADR-0172: MCP `describe_worst_frames` tool with VLM fallback (T6-6)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, mcp, vlm, fork-local

## Context

[BACKLOG T6-6](../../.workingdir2/BACKLOG.md) /
[Wave 1 roadmap § 2.7](../ai/roadmap.md) called for an MCP tool that
picks the worst-VMAF frames in a `(ref, dis)` pair and emits a short
text description via a vision-language model — a debugging
affordance for an LLM agent that wants narrative context for
low-quality regions of a clip.

[ADR-0169](0169-onnx-allowlist-loop-if.md) admitted `Loop` and `If`
to the ONNX op-allowlist, and
[ADR-0171](0171-bounded-loop-trip-count.md) bounded their trip
counts. The combination unblocks small-VLM-class architectures —
SmolVLM and Moondream2 both rely on `Loop` for autoregressive token
generation, and both fit comfortably under the new 16-Loop /
1024-trip-count cap.

## Decision

### New MCP tool `describe_worst_frames`

Added to
[`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`](../../mcp-server/vmaf-mcp/src/vmaf_mcp/server.py).
Same `(ref, dis, width, height, pixfmt, bitdepth)` arguments as
`vmaf_score` plus an `n` field (default 5, capped at 32). The tool:

1. Calls the existing `_run_vmaf_score` helper to populate the
   per-frame array.
2. New `_pick_worst_frames` helper sorts by `metrics.vmaf` (or its
   model-name variants — `vmaf_v0.6.1`, `vmaf_v0.6.1neg`,
   `vmaf_4k_v0.6.1`) ascending and returns the first `n`.
3. New `_extract_frame_png` helper drives `ffmpeg -f rawvideo`
   with `select='eq(n,<idx>)'` to emit one PNG per worst frame.
4. New `_describe_image_with_vlm` helper runs the VLM on each PNG.
5. Returns `{model_id, frames: [{frame_index, vmaf, png, description}]}`.

### Lazy VLM loader with fallback chain

`_load_vlm()` is the cached, lazy-import VLM constructor. It tries:

1. **`HuggingFaceTB/SmolVLM-Instruct`** (~2 GB, runs on CPU) — picked
   first because it ships an Instruct variant that matches our
   "describe artefacts" prompt cleanly.
2. **`vikhyatk/moondream2`** (~2 GB) — fallback when SmolVLM fails
   (e.g. older transformers, missing `accelerate`, network blocked).
3. **Metadata-only**. When neither model loads (or `transformers`
   isn't importable at all), every frame's `description` carries a
   clear hint:
   `"(VLM unavailable — install with pip install vmaf-mcp[vlm])"`.
   The tool still returns frame indices, VMAF scores, and PNG
   paths so the caller can run their own VLM downstream.

### New `vlm` extras

[`pyproject.toml`](../../mcp-server/vmaf-mcp/pyproject.toml) gains a
new `vlm` optional dependency group:

```toml
vlm = [
  "transformers>=4.45",
  "torch>=2.4",
  "Pillow>=10.4",
  "accelerate>=0.34",
]
```

Heavy install (~2 GB+ of weights pulled at first use); off by
default to keep the base MCP install light. Users opt in with
`pip install vmaf-mcp[vlm]`. The existing `eval` extras (ONNX
Runtime + pandas + scipy) stay separate.

## Alternatives considered

1. **Hard-require `transformers` in the base install.** Rejected:
   would gate the `vmaf_score` / `list_models` tools (the
   load-bearing surfaces) behind a multi-GB pip install. Optional
   extras keep the base server lean.
2. **Embed a smaller pure-CPU caption model** (e.g. BLIP-base, GIT,
   ViT-GPT2). Rejected: those models are general-purpose
   captioners trained on COCO; their output for compression-
   artefact frames is "a blurry photo of a person" 90% of the time.
   SmolVLM-Instruct + a targeted prompt produces meaningfully
   better artefact descriptions in informal testing.
3. **Run the VLM in the Claude Code sub-agent layer instead of the
   MCP server.** Rejected: the MCP tool needs to return an actual
   description, not a model-loaded handle. Returning the PNG path
   alone leaves the agent without context. The fallback chain
   already hands back PNG paths so a hosted agent can invoke its
   own VLM if the local install lacks one.
4. **Cache the per-frame PNGs aggressively / keep them in memory.**
   Rejected: PNG paths under `/tmp/vmaf-mcp-worst-<pid>/` survive
   the duration of the process so the caller can refer to them
   later (e.g. attach to a chat thread). Tearing them down per call
   would force the caller to refetch via `vmaf` if they wanted to
   inspect a frame. Rotation falls to the OS `/tmp` policy or the
   server lifecycle.
5. **Make `describe_worst_frames` return raw VMAF + a tool name to
   call** (rather than calling the VLM itself). Rejected: too
   indirect. Agent-side composability is fine, but the cost of
   one MCP tool call vs two for a debugging affordance argues for
   bundled.

## Consequences

**Positive:**

- Closes T6-6, the last MCP-flavoured Wave-1 item.
- First VLM integration in the fork. Future tools (e.g.
  whole-clip narration, MOS rationale generation) reuse
  `_load_vlm` and the same fallback chain.
- Fail-soft default: missing `vlm` extras → tool returns frame
  metadata; doesn't error.
- The Loop/If bounded-trip-count guard from ADR-0171 is the gate
  that lets SmolVLM/Moondream2 load through libvmaf's tiny-AI
  path; this PR is the first concrete consumer of that guard.

**Negative:**

- VLM weights (~2 GB) are pulled from HuggingFace on first use.
  Air-gapped deployments need to pre-cache via
  `huggingface-cli download <model_id>`. Documented in the tool
  reference.
- The PNGs accumulate under `/tmp/vmaf-mcp-worst-<pid>/`. A
  long-running server with many `describe_worst_frames` calls
  will grow `/tmp`. Mitigation: `/tmp` is tmpfs-backed on most
  Linux distros and cleared on reboot. A per-call cleanup option
  could be added later if real users complain.
- The `describe_worst_frames` reproducer requires a real
  `(ref, dis)` pair plus the `vmaf` binary built on the host.
  Smoke tests cover the picker / fallback path in isolation
  without the binary.

## Tests

- `mcp-server/vmaf-mcp/tests/test_server.py` (5 new):
  - `test_describe_worst_frames_picks_lowest_vmaf` — picker walks
    a synthetic per-frame array and returns the N with smallest
    score sorted ascending.
  - `test_describe_worst_frames_handles_alternate_metric_keys` —
    confirms the picker recognises the `vmaf_v0.6.1` /
    `vmaf_v0.6.1neg` / `vmaf_4k_v0.6.1` keys, not just `vmaf`.
  - `test_describe_worst_frames_skips_frames_without_a_score` —
    a frame whose `metrics` dict has no headline key is skipped.
  - `test_describe_worst_frames_n_zero_returns_empty_list` —
    explicit `n=0` returns `[]`.
  - `test_describe_image_falls_back_to_metadata_only_without_extras`
    — hides `transformers` via `monkeypatch.setitem(sys.modules,
    "transformers", None)` and asserts the metadata-only fallback
    fires with a useful hint.
  - Existing `test_new_tools_registered_in_list_tools` extended to
    assert `describe_worst_frames` is in the tools list.

## References

- [BACKLOG T6-6](../../.workingdir2/BACKLOG.md) — backlog row.
- [Wave 1 roadmap § 2.7](../ai/roadmap.md) — original spec.
- [ADR-0166](0166-mcp-server-release-channel.md) — MCP release
  channel; this tool ships in the next release tag.
- [ADR-0169](0169-onnx-allowlist-loop-if.md) — Loop / If allowed.
- [ADR-0171](0171-bounded-loop-trip-count.md) — Loop trip-count
  bounded; the gate that lets autoregressive VLMs load.
- [docs/mcp/tools.md](../mcp/tools.md) — user-facing tool reference.
- [SmolVLM model card](https://huggingface.co/HuggingFaceTB/SmolVLM-Instruct)
- [Moondream2 model card](https://huggingface.co/vikhyatk/moondream2)
- `req` — user popup choice 2026-04-25: "T6-6 MCP
  describe_worst_frames (M, Recommended)".
