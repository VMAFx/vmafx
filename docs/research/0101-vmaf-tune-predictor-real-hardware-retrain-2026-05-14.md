# Research 0101: vmaf-tune Predictor Real Hardware Retrain (2026-05-14)

## Question

Which `vmaf-tune` predictor stubs can be replaced by real weights with
data already present on this machine?

## Findings

- The shipped `model/predictor_<codec>.onnx` family had synthetic-stub
  cards for every codec adapter.
- The local Phase-A hardware sweep
  `runs/phase_a/full_grid/comprehensive.jsonl` contains real measured
  rows for six hardware codecs: `h264_nvenc`, `hevc_nvenc`,
  `av1_nvenc`, `h264_qsv`, `hevc_qsv`, and `av1_qsv`.
- That corpus predates the canonical trainer key names and uses
  `codec`, `q`, `vmaf`, and `actual_kbps`; the trainer only consumed
  `encoder`, `crf`, `vmaf_score`, and `bitrate_kbps`.
- There is no equivalent local corpus for `libx264`, `libx265`,
  `libsvtav1`, `libaom-av1`, `libvvenc`, or the AMF adapters in this
  pass, so flipping those would either keep synthetic targets or require
  a new encode sweep.

## Decision

Teach the trainer to normalize the existing hardware-sweep aliases and
retrain only the six codecs with real measured rows. Keep remaining
predictors as explicit synthetic stubs until their corpora are built.

## Results

Command:

```bash
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m vmaftune.predictor_train \
  --corpus runs/phase_a/full_grid/comprehensive.jsonl \
  --output-dir model \
  --codec h264_nvenc --codec hevc_nvenc --codec av1_nvenc \
  --codec h264_qsv --codec hevc_qsv --codec av1_qsv \
  --epochs 200 --batch-size 64
```

Held-out metrics:

| Codec | Rows | PLCC | SROCC | RMSE |
| --- | ---: | ---: | ---: | ---: |
| `h264_nvenc` | 2592 | 0.7908 | 0.7837 | 13.7288 |
| `hevc_nvenc` | 2592 | 0.7439 | 0.7374 | 12.0813 |
| `av1_nvenc` | 2592 | 0.6556 | 0.6154 | 12.4924 |
| `h264_qsv` | 1620 | 0.7935 | 0.8551 | 12.9500 |
| `hevc_qsv` | 1620 | 0.8305 | 0.8320 | 9.7751 |
| `av1_qsv` | 1620 | 0.8777 | 0.8424 | 8.5336 |

The metrics are intentionally not made to look stub-perfect. These are
real-corpus replacements with honest validation numbers; the mandatory
verify pass in `vmaf-tune fast` remains the guardrail.

## Alternatives Considered

| Option | Pros | Cons | Decision |
| --- | --- | --- | --- |
| Flip all 14 predictors in one PR | Retires the whole predictor stub family | Would require new software/AMF encode sweeps or fake targets | Rejected |
| Convert the corpus with an external script | No trainer change | Adds a one-off format shim future agents must rediscover | Rejected |
| Normalize aliases in the trainer and flip six hardware models | Uses existing real data, repeatable command, small code change | Leaves software/AMF stubs for follow-up | Chosen |

## References

- req: "then well yeah do the real weights, we can train everything on this machine"
- req: "more than half of tune is a scaffold"
- `runs/phase_a/full_grid/comprehensive.jsonl` (local, gitignored)
- `tools/vmaf-tune/src/vmaftune/predictor_train.py`
