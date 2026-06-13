<!-- markdownlint-disable MD060 -->
# ADR-0520: Wire `vmaf --no-reference` through to the scoring path

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Opus 4.7)
- **Tags**: cli, ai, dnn, docs

## Context

The `--no-reference` CLI flag has been advertised in `vmaf --help` and
documented in `docs/usage/cli.md` since the tiny-AI surface landed, but
the wiring was incomplete: `cli_parse.c` parsed the flag into
`CLISettings::no_reference` and then ignored it. The unconditional
`if (!settings->path_ref)` gate at the end of CLI validation rejected
every NR invocation with `Reference .y4m or .yuv (-r/--reference) is
required`, contradicting the documented intent that NR-mode runs only
need a distorted source plus a no-reference tiny model
(`.workingdir/bbb_reports/E2E_TEST_MATRIX_v9.md` Finding 8).

A real-world NR pipeline (e.g. ingest-side quality monitoring where
the original is unavailable) cannot construct a reference YUV at all,
so the CLI surface MUST honour the flag or the documented capability is
a fiction.

## Decision

We will wire `--no-reference` through the CLI end-to-end:

1. `cli_parse.c`: when `--no-reference` is set, replace the "reference
   required" check with a `--tiny-model required` check (the only
   downstream scorer that can produce a score without a reference is a
   tiny ONNX model). Also auto-suppress the built-in VMAF SVM default
   that the parser would otherwise inject (`model_cnt == 0 &&
   !no_prediction` branch), because the SVM features
   (`vif`, `adm`, `motion`) are FR by construction and would fail at
   the picture-pair gate.

2. `vmaf.c`: when `c.no_reference` is set, do not require a reference
   path. Open the distorted source twice (two `FILE*` + two
   `video_input` handles backed by the same file) so each frame
   produces two independent picture-pool entries. Feed them to
   `vmaf_read_pictures(vmaf, pic_dist_a, pic_dist_b, idx)`; the public
   API enforces a non-null pair, and an SVM-free, NR-tiny-model-only
   feature graph emits no FR extractor work, so the second copy is
   only seen by `vmaf_picture_unref` in the cleanup tail.
   `vmaf_ctx_dnn_run_frame` (the rank-4 path in `libvmaf.c`) already
   reads picture data exclusively from the `ref` argument, so the
   distorted frame reaches the NR tensor bridge unchanged.

3. Rank-2 feature-vector tiny models (ADR-0518) are intentionally
   refused in NR mode at parse time — they consume classic VMAF feature
   columns (`adm2`, `vif_*`, `motion2`) which require a real reference.

4. Skip the `report_pooled_scores` path entirely in NR mode (the
   classic SVM is not loaded, so there is nothing to pool); the tiny
   model's per-frame scores still land in the JSON / XML / CSV report
   via the feature-collector path that `vmaf_ctx_dnn_run_frame_nchw`
   already writes through.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add a public `vmaf_read_pictures_nr(vmaf, dist, idx)` entry point | Single picture per frame; no double-decode | Touches public API surface; needs ABI-bump, header export, ffmpeg-patch wiring, MCP wiring; out of scope for a bug-fix PR | Deferred to T6-NR-PUBLIC-API follow-up if NR throughput becomes a concern |
| Synthesize a zero-buffer "reference" picture in the CLI | Avoids opening dist twice | Allocates extra memory; semantically misleading; would also need to bypass `vmaf_picture_ref` (not public) | Worse trade-off than the open-twice approach |
| Refuse `--no-reference` and remove the flag | Cleanest possible code path | Breaks the documented surface; user already depends on it per the task brief | Rejected |

## Consequences

- **Positive**: `--no-reference` matches the documented behaviour; NR
  ingest pipelines can score distorted-only sources; the e2e test
  matrix's Finding 8 closes.
- **Positive**: No public-API change. ffmpeg-patch stack untouched
  (the filter does not surface NR-mode wiring today).
- **Negative**: NR mode double-decodes the distorted file because the
  CLI opens it via two `video_input` handles. For the typical
  per-frame inference cost this is in the noise; a public NR entry
  point can eliminate the redundancy when warranted.
- **Negative**: The picture pool needs one extra slot per worker to
  accommodate the second dist fetch. `pic_cnt` is sized as
  `2 * (thread_cnt + 1) + 1` and the NR path replaces the ref slot
  with a second dist slot, so the existing budget already covers it.
- **Neutral / follow-ups**: A future `vmaf_read_pictures_nr` API would
  let MCP / Python bindings expose NR scoring without going through
  the CLI; tracked as T6-NR-PUBLIC-API.

## References

- Bug report: `.workingdir/bbb_reports/E2E_TEST_MATRIX_v9.md` Finding 8
- Prior tiny-AI work: [ADR-0518](0518-tiny-model-loader-external-data-and-feature-rank.md)
- Source: `req` (direct user task brief, 2026-05-18, "Fix the
  `vmaf --no-reference` CLI flag which is a documented-but-no-op").
