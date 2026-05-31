<!-- markdownlint-disable MD060 -->
# Research 0109: `vmaf-tune` Bisect Sample-Clip Wiring

## Question

Close the Phase-B `vmaf-tune` gap where `compare` and the Python bisect
predicate still full-source encoded every iteration even though ADR-0301
sample-clip encode and score primitives already exist.

## Findings

- `EncodeRequest` already carries `sample_clip_seconds` and
  `sample_clip_start_s`, and every registered encode adapter maps those
  fields to FFmpeg input-side `-ss` / `-t`.
- `ScoreRequest` already carries `frame_skip_ref` and `frame_cnt`, so the
  scorer can evaluate the matching reference window without materialising
  a clipped YUV file.
- `vmaf-tune compare` builds its default real predicate through
  `make_bisect_predicate`; adding one argument there is enough to expose
  the mode to multi-codec comparisons without changing the report schema.
- Existing corpus sample-clip behavior treats unknown duration or
  sample duration greater than the source as full-source mode. Phase-B
  bisect should keep that contract.

## Alternatives Considered

| Option | Decision | Reason |
|---|---|---|
| Materialise clipped reference YUV files before each bisect | Rejected | Adds disk I/O and cleanup without improving alignment; ADR-0301 already selected scorer frame skips as the zero-copy path. |
| Encode the first `N` seconds when source duration is unknown | Rejected | `compare` documents centre samples. Without duration there is no centre anchor, so falling back to full-source mode matches corpus semantics. |
| Add a separate `vmaf-tune bisect` CLI first | Rejected | The shipped user surface is `compare`; a standalone command remains a separate backlog item. |
| Thread `sample_clip_seconds` through `bisect_target_vmaf` and `make_bisect_predicate` | Accepted | Keeps the existing adapter/scorer contracts, preserves centre-window alignment, and exposes the speedup through the current user-facing command. |

## Validation

```bash
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m pytest \
  tools/vmaf-tune/tests/test_bisect.py \
  tools/vmaf-tune/tests/test_compare.py -q
.venv/bin/ruff check \
  tools/vmaf-tune/src/vmaftune/bisect.py \
  tools/vmaf-tune/src/vmaftune/cli.py \
  tools/vmaf-tune/tests/test_bisect.py \
  tools/vmaf-tune/tests/test_compare.py
make format-check
```
