<!-- markdownlint-disable MD060 -->
# ADR-0499: vmaf-tune ladder must decode container/Y4M references before scoring

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: vmaf-tune, ladder, corpus, ffmpeg, docs

## Context

The BBB end-to-end smoke run on 2026-05-18 (post-PR #1255 / ADR-0498)
confirmed five of the v2 bug fixes still hold but surfaced one fresh
blocker:

- `vmaf-tune ladder --src <bbb>.mp4 …` exits 1 with
  `RuntimeError: default sampler produced no scorable encodes`. The
  encode succeeds, but the score step fails. Traced to
  `vmaftune.corpus._maybe_decode_distorted` only decoding the
  **distorted** leg before invoking the libvmaf CLI. The reference
  (`ScoreRequest.reference = job.source`) was handed to the binary
  as-is. The CLI then read the container as raw planar YUV and
  aborted with `yuv: file size mismatch …`. The bisect path
  (`vmaftune.bisect._predicate_for_codec`) already decodes the
  reference correctly (added in ADR-0498) so the gap was corpus-only.

A secondary finding: `_VMAF_RAW_SUFFIXES = {".yuv", ".y4m", ""}` lists
`.y4m` as "no decode needed". This is wrong. `vmaf-tune` always
passes `--width` / `--height` / `--pixel_format` / `--bitdepth` (see
`vmaftune.score.build_vmaf_command`) — each of those flags flips
`settings->use_yuv = true` in `core/tools/cli_parse.c` (lines
637–651 as of 2026-05-18) which routes both inputs through
`raw_input_open`. `.y4m` files therefore trip the file-size mismatch
guard the same way `.mp4` does. The empty-suffix entry is kept for
fixture trees that ship raw YUV without a `.yuv` suffix — geometry is
already pinned by the explicit flags, so those inputs round-trip
correctly.

A tertiary informational finding (V3-C): the dev-mcp container's
ffmpeg ships without `libsvtav1` (ADR-0496). `compare` already marks
the encoder as `ok=false` with `error="encoder unavailable
(libsvtav1): Encoder not found: libsvtav1"` via the bisect
discriminator added in ADR-0498 follow-up #6 — no code change
required; pinning the invariant via a regression test.

## Decision

Add a `_maybe_decode_reference` helper in `vmaftune.corpus` that
mirrors `_maybe_decode_distorted` for the reference leg. Invoke it
once per `iter_rows` call (before the cell loop) and reuse the
`.ref.decoded.yuv` sidecar across every (preset, crf) cell so the
cost is amortised across the sweep. Drop `.y4m` from both
`_VMAF_RAW_SUFFIXES` (corpus) and `VMAF_RAW_SUFFIXES` (score). When
the reference decode fails, short-circuit every cell with a clean
failed row instead of re-running ffmpeg + vmaf N times on a
path the binary cannot parse.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `_maybe_decode_reference` (chosen) | Mirrors existing distorted-leg helper; one decode per sweep; consistent with bisect's already-shipping approach. | Two helpers cover similar work — see `_decode_source_to_yuv` shared building block. | Default. |
| Pre-decode in CLI before constructing the job | One decode call, no helper; obvious to users. | Couples the CLI to encode-time concerns; breaks the corpus library's "give me a source path, I'll handle it" contract; complicates per-shot / ladder / compare entry points which all build their own jobs. | Coupling cost too high. |
| Teach libvmaf CLI to accept `.y4m` properly when `--width` is set | Removes the need for any decode wrapper for Y4M. | Out of scope for a vmaf-tune-side fix; would still need decode for `.mp4` / `.mkv`; gates on a separate libvmaf PR. | Doesn't close the blocker. |
| Drop the empty-suffix entry too | More restrictive; harder to mis-use. | Breaks fixture trees that name raw YUV without `.yuv` (e.g. Netflix golden-data convention). | Preserves operator habit. |

## Consequences

- **Positive**: `vmaf-tune ladder` works on container and Y4M
  sources end-to-end. Symmetry between corpus + bisect score paths
  removes a class of "works in bisect, fails in ladder" surprises.
  Suffix table now reflects what the libvmaf CLI actually accepts.
- **Negative**: One extra ffmpeg invocation per `iter_rows` call when
  the source is a container. Dominated by the encode-time budget
  (typical sweep encodes O(10) cells at O(seconds) each vs a single
  O(seconds) decode), so per-sweep overhead is < 5 %.
- **Neutral / follow-ups**:
  - `test_bbb_e2e_v3_bug_cluster.py` pins the new invariants.
  - `test_vmaf_raw_suffixes_matches_libvmaf_cli_source` cross-checks
    the suffix table against `cli_parse.c` so an upstream change to
    the CLI's use_yuv discipline is caught at lint-test time.
  - The bisect path's reference decode (ADR-0498) is pinned by a
    second test in the same file as an anti-regression guard.

## References

- BBB e2e v3 bug log: `/tmp/bbb_e2e_bugs_v3.md` (gitignored)
- Predecessor: [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md) (v2 cluster)
- Earlier predecessor: [ADR-0497](0497-vmaf-tune-bbb-e2e-bug-cluster.md) (v1 cluster)
- Container build policy: [ADR-0496](0496-prefer-dev-mcp-container-rule.md)
- libvmaf CLI source: `core/tools/cli_parse.c` (lines 637–651)
- Source: `req` (direct user direction in the agent dispatch
  briefing on 2026-05-18 — paraphrased: "ladder fails because only
  the distorted leg is decoded; mirror that fix to the reference
  leg, drop .y4m from the raw-suffix set, and verify against the
  libvmaf CLI source").
