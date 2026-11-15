<!-- markdownlint-disable MD013 MD046 MD060 -->
# Tutorial 1 — Score a pair

Ten minutes. At the end you have a JSON report with the pooled VMAF score of
the fixture pair, and you know how to read it.

## What you need

A CPU build of the `vmaf` binary ([getting started](../getting-started/index.md))
and the fixture pair in `testdata/` — `ref_576x324_48f.yuv` (the reference)
and `dis_576x324_48f.yuv` (the distorted one). Raw `.yuv` files carry no
header, so the geometry travels on the command line: 576x324, 48 frames,
yuv420p, 8-bit.

## Score the pair

The pinned model is `vmaf_v0.6.1` — the model the golden-data tests use, so
the numbers in these tutorials match your run
([why the default model differs](../reference/concepts.md#model-mismatch)).

<!-- run-check:t1-score exec=cli -->
```bash
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 \
    -m version=vmaf_v0.6.1 --backend cpu --json -o score.json
```

Takes a few seconds for the 48 frames. No output on success — the result is
in `score.json`.

## Read the result

```bash
python3 -c "import json; d = json.load(open('score.json')); \
print(d['pooled_metrics']['vmaf'])"
```

```text
{'min': 93.129024, 'max': 100.0, 'mean': 94.32301, 'harmonic_mean': 94.284562}
```

The **mean** (94.32301) is the number most tools quote — it is the pooled
VMAF score of the pair, and this exact value is recorded in
`testdata/scores_cpu_576.json`, the CPU golden snapshot your run just
reproduced. `min` is the worst frame, `harmonic_mean` weights the bad frames
more ([pooling](../reference/glossary.md#pooling)).

## What just happened

- `-r` / `-d` take the **reference first, distorted second** — the opposite
  of FFmpeg's filter pads, a classic wrong-number trap
  ([input ordering](../usage/ffmpeg.md#input-ordering)).
- `-m version=vmaf_v0.6.1` pins the model; without it you get the fork
  default `vmaf_v1.0.16_3d0h` and a different number
  ([default model](../reference/glossary.md#default-model)).
- `--backend cpu` keeps the run on the CPU, matching the snapshot's
  `backend_used`.
- `--json -o score.json` writes the full report: per-frame scores in
  `frames`, the four aggregates in `pooled_metrics`
  ([CLI reference](../usage/cli.md)).

## Try one thing

Score the pair again without `--backend cpu` — on a GPU machine the backend
auto-selects and the frames take a different path
([backends](../usage/cli.md#selection-flags)); on a CPU-only machine nothing
changes. Then delete `score.json` and continue to
[Compare two encodes](compare-encodes.md).
