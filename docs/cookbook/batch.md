<!-- markdownlint-disable MD013 MD046 MD060 -->
# Recipe — Score a batch of pairs

**Situation:** many pairs, one report each — a regression sweep, a corpus
run. The flag surface does not change; the shell does the looping, the
report naming does the bookkeeping.

## The tree convention

Name the pairs so a glob finds them: `pairs/<case>.ref.yuv` and
`pairs/<case>.dis.yuv`, all sharing one geometry. The in-tree fixture pair
gives you two "cases" if you fake a tree from the two resolutions:

<!-- run-check:batch-tree exec=cli -->
```bash
mkdir -p pairs && cp testdata/ref_576x324_48f.yuv pairs/r576.ref.yuv && \
cp testdata/dis_576x324_48f.yuv pairs/r576.dis.yuv && \
cp testdata/ref_640x480_48f.yuv pairs/r640.ref.yuv && \
cp testdata/dis_640x480_48f.yuv pairs/r640.dis.yuv
```

## The loop

```bash
#!/usr/bin/env bash
# batch.sh PAIRS_DIR W H P — one JSON report per pair under reports/
set -euo pipefail
DIR=$1; W=$2; H=$3; P=$4
mkdir -p reports
for ref in "$DIR"/*.ref.yuv; do
    case=$(basename "$ref" .ref.yuv)
    vmaf -r "$ref" -d "$DIR/$case.dis.yuv" \
        -w "$W" -h "$H" -p "$P" -b 8 \
        -m version=vmaf_v0.6.1 --backend cpu \
        --json -o "reports/$case.json"
done
python3 - <<'EOF'
import json, glob
for path in sorted(glob.glob("reports/*.json")):
    mean = json.load(open(path))["pooled_metrics"]["vmaf"]["mean"]
    print(f"{path}: {mean:.6f}")
EOF
```

`bash batch.sh pairs 576 324 420` (then `bash batch.sh pairs 640 480 420`
for the second geometry) prints one mean per case.

## Reading it

- The 576 case reproduces **94.32301**; the 640 case has its own snapshot in
  `testdata/scores_cpu_640.json` — both are the CPU golden pairs, so your
  batch run doubles as a toolchain sanity check.
- One geometry per batch invocation: a batch that mixes resolutions needs
  the geometry carried per pair (encode it in the filename and split the
  glob), because raw YUV has no header.
- Keep the model and `--backend` fixed across the batch; mixing models
  inside one table is the classic
  [model mismatch](../reference/concepts.md#model-mismatch).

See the [glossary](../reference/glossary.md) — terms used here:
[default model](../reference/glossary.md#default-model).
