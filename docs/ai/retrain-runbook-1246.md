<!-- markdownlint-disable MD013 MD024 MD031 MD033 MD060 -->
# Tiny-AI Retraining Runbook — `vmaf_v1.0.16_3d0h` Teacher (Epic #1246)

This runbook defines the end-to-end operational procedure for the one-shot
retraining pass of the fork's tiny-AI models against the canonical
`vmaf_v1.0.16_3d0h` teacher model (Epic [#1246](https://github.com/VMAFx/vmafx/issues/1246)).

The retrain executes **once** when all preceding 1.0.0 epics are closed and
preconditions are satisfied.

---

## 1. Governing Policies and Maintainer Decisions

All procedures in this runbook strictly enforce the binding maintainer decisions
and architectural records:

- **Maintainer decision, 2026-09-04 (D1 — Student Features)**: Raw extraction collects
  the union feature pool (`FULL_FEATURES` + `adm3`, 26 raw features per ADR-1173).
  The shipped student model contract remains locked to canonical-6 (`adm2`,
  `vif_scale0`..`vif_scale3`, `motion2`) for release 1.0.0 unless post-run sweeps
  conclusively demonstrate an accuracy advantage for a wider subset.
- **Maintainer decision, 2026-09-04 (D2 — Training Corpora)**: Retraining consumes all
  corpora — Netflix Public, CHUG UGC-HDR, BVI-DVC (Part 1, tiers A–D), YouTube UGC,
  and all 152,265 KoNViD-150k clips. The maintainer accepts an estimated wall-clock
  duration of ~130 h (K150K alone requires ~105–110 h at 0.36–0.40 clip/s on an
  RTX 4090). This supersedes the initial 76–82 h estimate in Epic #1246.
- **Maintainer decision, 2026-09-04 (D3 — Single Teacher Model)**: A single teacher model,
  `vmaf_v1.0.16_3d0h`, is dispatched for every teacher-scored row. No `vmaf_hdr_v0.6.1`
  substitution is permitted in the retrain path. HDR rows train the subjective MOS
  head only.
- **Maintainer decision, 2026-09-04 (D4 — Geometry Refusals)**: Clips rejected by
  `libvmaf` geometry limits (<216 px on width or height for 4:2:0, or chroma <80×80)
  are dropped, counted in the extraction manifest's `fail` tally, and bypassed. A
  secondary or fallback teacher model is never invoked.
- **Maintainer decision, 2026-09-04 (D5 — Quantisation Target)**: Int8 static PTQ or
  QAT in QDQ wire format is the shipped release format. FP32 is the canonical
  training export and regression baseline. Shipped models are gated on measured drop
  thresholds (Research-2029).
- **Maintainer decision, 2026-09-04 (D6 — Hardware Backends)**: Feature extraction is
  restricted to CPU and CUDA. The SYCL lane remains disabled (`--no_sycl`) until
  the SYCL v1-model fix (PR [#1307](https://github.com/VMAFx/vmafx/pull/1307)) and
  open motion2/cambi drift rows are merged and verified.
- **Ensemble Policy (ADR-1105)**: The five `fr_regressor_v2_ensemble_v1_seed{0..4}`
  rows remain parked at `smoke: true` until this run executes
  `train_fr_regressor_v2_ensemble_loso.py` and `export_ensemble_v2_seeds.py` against
  the retrained corpus at `codec_vocab = 6`.
- **Golden Data Invariant (ADR-0024)**: Netflix golden-data assertions in `python/test/`
  (`assertAlmostEqual` values) are never modified.

---

## 2. Preconditions and Gating Verification

Before initiating any extraction or training command, the operator must execute
the verification command for each gate and confirm a passing result.

| Gate | Requirement | Verification Command | Gate Status Today |
|---|---|---|---|
| **G1** | Every other 1.0.0 epic closed | `gh issue list --milestone "1.0.0 — First release" --state open` | **FAIL** — 12 open besides this one: #1235, #1236, #1237, #1238, #1240, #1241, #1242, #1243, #1244, #1245, #1270, #1272. Note the epic bodies are snapshots and several items in them have already shipped, so the count overstates the remaining work; each needs auditing against the code before it is treated as outstanding. |
| **G2** | `master` fully green across CI matrix | `gh run list --branch master --limit 20 --json conclusion,name --jq '[.[]\|select(.conclusion=="failure")]'` | **PASS** as of 2026-09-06 — zero failing runs on `master` HEAD. The release-please failure this row was written for is gone: it was the missing release-bot App credential, warned-not-errored on push by ADR-1171, and the workflow now reports success. |
| **G3** | Container rebuilt with GPU default-model fixes | See §3 for container rebuild & CUDA verification command | **PASS** as of 2026-09-06 — #1307, #1312 and #1324 are all on `master`, the container was rebuilt from `cd52f2670` and the default model was verified on **all four** backends, not just CUDA: CPU 82.816062, CUDA 82.814062, SYCL 82.814061, HIP 82.816061, every one exiting 0. Evidence and container digest in [issue #1246 comment](https://github.com/VMAFx/vmafx/issues/1246#issuecomment-5555646084). |
| **G4** | K150K re-smoke verified with zero disk leak | See §4 for 5-clip smoke & manifest validation command | **FAIL** — still blocked on [#1302](https://github.com/VMAFx/vmafx/pull/1302), which adds the `--vmaf-model` flag and the row-level `teacher_model` stamping §4.2 asserts. Confirmed absent from `master`: `git show origin/master:ai/scripts/extract_k150k_features.py \| grep -c vmaf.model` returns 0. #1302 itself has **no failing check** — its only red mark is the aggregator's draft guard ("Draft PRs must not satisfy Required Checks Aggregator"), and its ADR-0108 deliverables validator passes six of six. It needs promotion, not repair. |
| **G5** | Explicit maintainer authorization | `gh issue view 1246 --comments` | **FAIL** (Awaiting maintainer sign-off) |

> **Gate status is a measurement, not a plan.** Every cell above says how it was
> checked and when. G2 and G3 moved to PASS on 2026-09-06 because they were
> re-run, not because the work was assumed done; G1 and G4 stayed FAIL for the
> same reason. Re-run the verification command before trusting any row — the
> table this replaced had G3 failing on two PRs that had already merged.

---

## 3. Container Rebuild and Default-Model CUDA Verification

Canonical builds are produced exclusively inside the `vmaf-dev-mcp` container per
[ADR-1102](../adr/1102-container-only-publishing.md).

### 3.1 Rebuild Container

Execute on the host workstation:

```bash
docker compose --project-directory $(git rev-parse --show-toplevel) \
  -f dev/docker-compose.yml build dev-mcp && \
docker compose -f dev/docker-compose.yml up -d
```

### 3.2 Verify CUDA Binary with `vmaf_v1.0.16_3d0h`

Execute inside `vmaf-dev-mcp` to prove the CUDA backend scores accurately under
the default v1 model without segfaults or context leaks:

```bash
docker exec vmaf-dev-mcp /usr/local/bin/vmaf \
  --backend cuda \
  --model version=vmaf_v1.0.16_3d0h \
  -r /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
  -d /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
  -w 576 -h 324 -p 420 -b 8 \
  --json -o /tmp/smoke_cuda_v1.json

# Assert exit code 0 and pooled VMAF score aligns with v1 baseline (~82.816)
docker exec vmaf-dev-mcp python3 -c '
import json
with open("/tmp/smoke_cuda_v1.json") as f:
    data = json.load(f)
score = data["pooled_metrics"]["vmaf"]["mean"]
print(f"CUDA v1 score: {score:.6f}")
assert 82.80 <= score <= 82.83, f"Unexpected score: {score}"
'

# Clean up smoke output
docker exec vmaf-dev-mcp rm -f /tmp/smoke_cuda_v1.json
```

---

## 4. K150K Extraction Re-Smoke (≤5 Clips)

Verify `extract_k150k_features.py` under the rebuilt binary and confirm row-level
teacher stamping and manifest generation before launching the multi-day run.

*(Note: `--vmaf-model` and row-level teacher stamping require PR [#1302](https://github.com/VMAFx/vmafx/pull/1302)).*

### 4.1 Execute Smoke Command

```bash
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/extract_k150k_features.py \
  --clips-dir /workspace/.corpus/konvid-150k/clips \
  --scores /workspace/.corpus/konvid-150k/k150ka_scores.csv \
  --vmaf-bin /usr/local/bin/vmaf \
  --vmaf-model version=vmaf_v1.0.16_3d0h \
  --out /tmp/k150k_smoke.parquet \
  --manifest-out /tmp/k150k_smoke.manifest.json \
  --threads 2 \
  --threads-cuda 2 \
  --scratch-dir /tmp/k150k_smoke_scratch \
  --allow-fr-from-nr \
  --limit 5
```

> **Corpus paths, checked against the workstation on 2026-09-06.** Both this
> command and the §5.1 production extraction previously named
> `--scores .../scores.csv`, which does not exist. KoNViD-150k ships its scores
> split by part: the corpus holds `k150ka_scores.csv` and `k150kb_scores.csv`
> (plus matching `*_votes.csv` and a `manifest.csv`). `k150ka_scores.csv` is also
> what `extract_k150k_features.py` defaults to, and the script fails closed on a
> missing file — `error: scores CSV not found` — so the old path would have
> aborted the smoke on its first line, and the multi-day run with it.
>
> `--clips-dir` is left as `clips/`: a real directory of 153,841 files and a
> superset of the script's own default `k150ka_extracted/` (152,265). Lookup is
> by `video_name`, so either resolves. Verify both paths exist before committing
> to the long run rather than discovering it hours in:
>
> ```bash
> docker exec vmaf-dev-mcp ls -d /workspace/.corpus/konvid-150k/clips \
>   /workspace/.corpus/konvid-150k/k150ka_scores.csv
> ```

### 4.2 Validate Manifest Fields and Schema

Verify the manifest records `teacher_model == "vmaf_v1.0.16_3d0h"` and complete status:

```bash
docker exec vmaf-dev-mcp python3 -c '
import json, pyarrow.parquet as pq
with open("/tmp/k150k_smoke.manifest.json") as f:
    m = json.load(f)
assert m["schema"] == "k150k-feature-extraction-manifest-v1", f"Bad schema: {m.get(\"schema\")}"
assert m["status"] == "complete", f"Bad status: {m.get(\"status\")}"
assert m["teacher_model"] == "vmaf_v1.0.16_3d0h", f"Bad teacher: {m.get(\"teacher_model\")}"
assert m["stats"]["ok"] <= 5, f"Unexpected ok count: {m[\"stats\"][\"ok\"]}"
table = pq.read_table("/tmp/k150k_smoke.parquet")
assert "teacher_model" in table.column_names, "Missing teacher_model column"
assert set(table["teacher_model"].to_pylist()) == {"vmaf_v1.0.16_3d0h"}
assert "adm3_mean" in table.column_names, "Missing adm3 in raw extraction"
print("K150K smoke verification: PASS")
'
```

### 4.3 Clean Up Smoke Artifacts

Ensure **nothing survives on disk**:

```bash
docker exec vmaf-dev-mcp rm -rf \
  /tmp/k150k_smoke.parquet \
  /tmp/k150k_smoke.manifest.json \
  /tmp/k150k_smoke.parquet.done \
  /tmp/k150k_smoke.parquet.staging.jsonl \
  /tmp/k150k_smoke_scratch
```

---

## 5. Per-Corpus Feature Extraction

In accordance with maintainer decision D6 and the worktree/device discipline
([AGENTS.md](../../AGENTS.md) §12), **devices are never multiplexed**.
The RTX 4090 is dedicated to K150K extraction, while the Zen 5 CPU handles the
remaining corpora in parallel. All invocations run under `nohup` with stdout/stderr
redirected to dedicated logs under `runs/logs/`.

Create the logging and run directories first:

```bash
mkdir -p runs/logs runs/shards
```

### 5.1 K150K Corpus (CUDA-Pinned)

- **Target**: 152,265 clips (KoNViD-150k-A).
- **Backend**: CUDA (RTX 4090, outer parallelism `--threads-cuda 8`, inner `--threads 2`).
- **Wall-clock Estimate**: ~105–110 h.
  - *Basis*: Measured throughput of 0.36–0.40 clip/s with 8 outer workers on RTX 4090.
    $152{,}265 \text{ clips} \div 0.38 \text{ clip/s} \approx 400{,}700 \text{ s} \approx 111.3 \text{ h}$.
- **Prerequisite**: Requires PR [#1302](https://github.com/VMAFx/vmafx/pull/1302) (`--vmaf-model` flag).

```bash
nohup docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/extract_k150k_features.py \
  --clips-dir /workspace/.corpus/konvid-150k/clips \
  --scores /workspace/.corpus/konvid-150k/k150ka_scores.csv \
  --vmaf-bin /usr/local/bin/vmaf \
  --vmaf-model version=vmaf_v1.0.16_3d0h \
  --out /workspace/runs/shards/k150k_v1_features.parquet \
  --manifest-out /workspace/runs/shards/k150k_v1_features.manifest.json \
  --threads 2 \
  --threads-cuda 8 \
  --scratch-dir /tmp/k150k_scratch \
  --allow-fr-from-nr \
  > runs/logs/extract_k150k.log 2>&1 &
```

### 5.2 Netflix Public Corpus (CPU-Pinned)

- **Target**: 79 (ref, dis) pairs across 9 sources.
- **Backend**: CPU (`core/build-cpu/tools/vmaf` or `/usr/local/bin/vmaf --no_cuda --no_sycl`).
- **Wall-clock Estimate**: ~1.5–2 h.
  - *Basis*: 79 pairs × 48–100 frames at ~24 s/pair on Zen 5 CPU, plus residual speed feature extraction passes.
- **Prerequisite**: Requires PR [#1302](https://github.com/VMAFx/vmafx/pull/1302) (`--vmaf-model` flag).

```bash
nohup docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/extract_full_features.py \
  --data-root /workspace/.corpus/netflix \
  --vmaf-bin /usr/local/bin/vmaf \
  --vmaf-model version=vmaf_v1.0.16_3d0h \
  --cache-dir /tmp/vmaf_nflx_cache \
  --out /workspace/runs/shards/full_features_netflix.parquet \
  --manifest-out /workspace/runs/shards/full_features_netflix.manifest.json \
  > runs/logs/extract_netflix.log 2>&1 &
```

### 5.3 BVI-DVC Corpus (CPU-Pinned)

- **Target**: 80 sequences × 4 tiers (D, C, B, A), CRF 35 x264 encodes.
- **Backend**: CPU (`--no_sycl`).
- **Wall-clock Estimate**: ~8–12 h.
  - *Basis*: 10-bit YCbCr 4:2:0 decode and scoring: Tier D (~1 h), C (~2 h), B (~3 h), A (~4 h) (Research-0082).
- **Prerequisite**: Requires PR [#1302](https://github.com/VMAFx/vmafx/pull/1302) (teacher model resolution).

```bash
nohup docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/bvi_dvc_to_full_features.py \
  --bvi-dir /workspace/.workingdir2/bvi-dvc-extracted \
  --tier all \
  --vmaf-bin /usr/local/bin/vmaf \
  --model version=vmaf_v1.0.16_3d0h \
  --out /workspace/runs/shards/full_features_bvi_dvc_all.parquet \
  --manifest-out /workspace/runs/shards/full_features_bvi_dvc_all.manifest.json \
  --scratch /tmp/bvi_scratch \
  --cache-dir /tmp/bvi_cache \
  --crf 35 \
  > runs/logs/extract_bvi_dvc.log 2>&1 &
```

### 5.4 YouTube UGC Corpus (CPU-Pinned)

- **Target**: ~1,500 pairs (capped to 10 s @ 30 fps = 300 frames/pair).
- **Backend**: CPU (`--threads 8`).
- **Wall-clock Estimate**: ~8–10 h.
  - *Basis*: ~1,500 clips at ~20 s/clip decode and multi-threaded CPU extraction across 8 threads (`docs/ai/youtube-ugc-ingestion.md`).
- **Prerequisite**: Requires PR [#1302](https://github.com/VMAFx/vmafx/pull/1302).

```bash
nohup docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/extract_ugc_features.py \
  --manifest /workspace/.workingdir2/youtube-ugc/manifest.csv \
  --yuv-dir /tmp/ugc_yuv_scratch \
  --vmaf-bin /usr/local/bin/vmaf \
  --model version=vmaf_v1.0.16_3d0h \
  --out-parquet /workspace/runs/shards/full_features_ugc.parquet \
  --manifest-out /workspace/runs/shards/full_features_ugc.manifest.json \
  --threads 8 \
  > runs/logs/extract_ugc.log 2>&1 &
```

### 5.5 CHUG UGC-HDR Corpus (CPU-Pinned)

- **Target**: 856 references × 6 ladder encodes = 5,136 distorted clips.
- **Backend**: CPU.
- **Wall-clock Estimate**: ~4–6 h.
  - *Basis*: Resolution scaling + feature extraction on 5,136 HDR clips (`docs/ai/chug-ingestion.md`).
- **Policy Note**: Per maintainer decision D3, HDR rows train the MOS head only. `vmaf_hdr_v0.6.1` substitution is prohibited.

```bash
nohup docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/chug_extract_features.py \
  --input /workspace/.workingdir2/chug/chug.jsonl \
  --clips-dir /workspace/.workingdir2/chug/clips \
  --output /workspace/runs/shards/full_features_chug.parquet \
  --full \
  --feature-set full \
  --vmaf-bin /usr/local/bin/vmaf \
  > runs/logs/extract_chug.log 2>&1 &
```

---

## 6. Mixed-Teacher Refusal and Parquet Combination

Every extracted shard carries a `teacher_model` column. Before combining,
the operator runs a strict refusal check. Any shard lacking the column or
containing values other than `vmaf_v1.0.16_3d0h` causes immediate failure.

### 6.1 Pre-Combine Verification Script

Run inside `vmaf-dev-mcp`:

```python
import pyarrow.parquet as pq
from pathlib import Path

shards = [
    Path("runs/shards/full_features_netflix.parquet"),
    Path("runs/shards/full_features_bvi_dvc_all.parquet"),
    Path("runs/shards/full_features_ugc.parquet"),
    Path("runs/shards/k150k_v1_features.parquet"),
]

for p in shards:
    if not p.is_file():
        raise FileNotFoundError(f"Missing required extraction shard: {p}")
    t = pq.read_table(p, columns=["teacher_model"])
    teachers = set(t["teacher_model"].to_pylist())
    if teachers != {"vmaf_v1.0.16_3d0h"}:
        raise ValueError(f"Mixed-teacher refusal fired on {p}: found {teachers}")
    print(f"PASS: {p} ({t.num_rows} rows stamped with vmaf_v1.0.16_3d0h)")
```

### 6.2 Combine Shards

*(Note: Requires PR [#1302](https://github.com/VMAFx/vmafx/pull/1302) for combiner teacher validation).*

```bash
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/combine_full_feature_parquets.py \
  --input netflix /workspace/runs/shards/full_features_netflix.parquet \
  --input bvi_dvc /workspace/runs/shards/full_features_bvi_dvc_all.parquet \
  --input ugc /workspace/runs/shards/full_features_ugc.parquet \
  --input k150k /workspace/runs/shards/k150k_v1_features.parquet \
  --out /workspace/runs/full_features_combined_v1.parquet \
  --manifest-out /workspace/runs/full_features_combined_v1.manifest.json
```

---

## 7. Model Retraining, Export, Quantisation, and Drop Gating

### 7.1 FP32 Model Retraining Commands

#### 7.1.1 `vmaf_tiny_v2`, `vmaf_tiny_v3`, `vmaf_tiny_v4`

Retrain the core tiny MLP regressors on the combined v1-teacher parquet:

```bash
# vmaf_tiny_v2 (mlp_small, 6->16->8->1, ~257 params)
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_vmaf_tiny_v2.py \
  --parquet /workspace/runs/full_features_combined_v1.parquet \
  --out-ckpt /workspace/runs/vmaf_tiny_v2.pt \
  --out-stats /workspace/runs/vmaf_tiny_v2_stats.json \
  --epochs 90

docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/export_vmaf_tiny_v2.py \
  --ckpt /workspace/runs/vmaf_tiny_v2.pt \
  --out-onnx /workspace/model/tiny/vmaf_tiny_v2.onnx \
  --out-sidecar /workspace/model/tiny/vmaf_tiny_v2.json

# vmaf_tiny_v3 (mlp_medium, 6->32->16->1, ~737 params)
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_vmaf_tiny_v3.py \
  --parquet /workspace/runs/full_features_combined_v1.parquet \
  --out-ckpt /workspace/runs/vmaf_tiny_v3.pt \
  --out-stats /workspace/runs/vmaf_tiny_v3_stats.json \
  --epochs 90

docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/export_vmaf_tiny_v3.py \
  --ckpt /workspace/runs/vmaf_tiny_v3.pt \
  --out-onnx /workspace/model/tiny/vmaf_tiny_v3.onnx \
  --out-sidecar /workspace/model/tiny/vmaf_tiny_v3.json

# vmaf_tiny_v4 (mlp_large, 6->64->32->16->1, ~2,849 params)
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_vmaf_tiny_v4.py \
  --parquet /workspace/runs/full_features_combined_v1.parquet \
  --out-ckpt /workspace/runs/vmaf_tiny_v4.pt \
  --out-stats /workspace/runs/vmaf_tiny_v4_stats.json \
  --epochs 90

docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/export_vmaf_tiny_v4.py \
  --ckpt /workspace/runs/vmaf_tiny_v4.pt \
  --out-onnx /workspace/model/tiny/vmaf_tiny_v4.onnx \
  --out-sidecar /workspace/model/tiny/vmaf_tiny_v4.json
```

#### 7.1.2 `fr_regressor_v1`, `fr_regressor_v2`, `fr_regressor_v3`

```bash
# fr_regressor_v1
docker exec vmaf-dev-mcp env PYTHONPATH=/workspace/ai/src /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_fr_regressor.py \
  --parquet /workspace/runs/full_features_combined_v1.parquet \
  --features canonical6 \
  --epochs 100 \
  --out-onnx /workspace/model/tiny/fr_regressor_v1.onnx \
  --out-sidecar /workspace/model/tiny/fr_regressor_v1.json

# fr_regressor_v2 (codec-aware)
docker exec vmaf-dev-mcp env PYTHONPATH=/workspace/ai/src /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_fr_regressor_v2.py \
  --corpus /workspace/runs/phase_a/full_grid/per_frame_canonical6.jsonl \
  --epochs 100 \
  --out-onnx /workspace/model/tiny/fr_regressor_v2.onnx \
  --out-sidecar /workspace/model/tiny/fr_regressor_v2.json

# fr_regressor_v3 (codec-aware, 16-slot vocab)
docker exec vmaf-dev-mcp env PYTHONPATH=/workspace/ai/src /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_fr_regressor_v3.py \
  --corpus /workspace/runs/phase_a/full_grid/per_frame_canonical6.jsonl \
  --epochs 100 \
  --out-onnx /workspace/model/tiny/fr_regressor_v3.onnx \
  --out-sidecar /workspace/model/tiny/fr_regressor_v3.json
```

#### 7.1.3 `nr_metric_v1` (No-Reference MOS Head)

```bash
docker exec vmaf-dev-mcp env PYTHONPATH=/workspace/ai/src /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_konvid_mos_head.py \
  --konvid-1k /workspace/.workingdir2/konvid-1k/konvid_1k.jsonl \
  --feature-parquet /workspace/runs/full_features_combined_v1.parquet \
  --device cuda \
  --out-onnx /workspace/model/tiny/nr_metric_v1.onnx
```

#### 7.1.4 `fr_regressor_v2_ensemble_v1_seed{0..4}` (ADR-1105)

```bash
docker exec vmaf-dev-mcp env PYTHONPATH=/workspace/ai/src /opt/vmaf-venv/bin/python /workspace/ai/scripts/train_fr_regressor_v2_ensemble_loso.py \
  --corpus /workspace/runs/phase_a/full_grid/per_frame_canonical6.jsonl \
  --out-dir /workspace/runs/ensemble_v2_real \
  --epochs 100
```

### 7.2 Int8 Quantisation Export (Static PTQ and QAT)

Per maintainer decision D5, **int8 static PTQ or QAT in QDQ wire format is the shipped release format**.

#### 7.2.1 Static PTQ (QDQ Format)

Static PTQ collects real activation ranges over a representative calibration slice
drawn from the feature table:

```bash
# Generate calibration set npz from parquet if not staged
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python -c '
import numpy as np, pyarrow.parquet as pq
df = pq.read_table("runs/full_features_combined_v1.parquet").to_pandas()
cols = ["adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2"]
calib = df[cols].dropna().sample(512, random_state=42).to_numpy(dtype=np.float32)
np.savez("runs/calib_canonical6.npz", features=calib)
'

# Quantize vmaf_tiny_v3
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/ptq_static.py \
  /workspace/model/tiny/vmaf_tiny_v3.onnx \
  --calibration /workspace/runs/calib_canonical6.npz \
  --output /workspace/model/tiny/vmaf_tiny_v3.int8.onnx

# Quantize vmaf_tiny_v4
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/ptq_static.py \
  /workspace/model/tiny/vmaf_tiny_v4.onnx \
  --calibration /workspace/runs/calib_canonical6.npz \
  --output /workspace/model/tiny/vmaf_tiny_v4.int8.onnx

# Quantize nr_metric_v1
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/ptq_static.py \
  /workspace/model/tiny/nr_metric_v1.onnx \
  --calibration /workspace/runs/calib_canonical6.npz \
  --output /workspace/model/tiny/nr_metric_v1.int8.onnx
```

#### 7.2.2 QAT Export (`learned_filter_v1`)

```bash
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/qat_train.py \
  --config /workspace/ai/configs/learned_filter_v1_qat.yaml \
  --output /workspace/model/tiny/learned_filter_v1.int8.onnx \
  --epochs-fp32 20 \
  --epochs-qat 10
```

### 7.3 Op-Allowlist Enforcement (`check-ops`)

All exported `.onnx` and `.int8.onnx` models must be verified against
`core/src/dnn/op_allowlist.c` before committing:

```bash
for m in model/tiny/*.onnx; do
  docker exec vmaf-dev-mcp env PYTHONPATH=/workspace/ai/src \
    /opt/vmaf-venv/bin/python /workspace/ai/src/vmaf_train/cli.py check-ops --model "/workspace/$m"
done
```

*Every model must report `allowlist OK` and exit with code 0.*

### 7.4 Quantisation Drop Gate (`measure_quant_drop.py`)

Execute the quantisation drop gate across all models:

```bash
docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/measure_quant_drop.py \
  --all \
  --out-json /workspace/runs/quant_drop_report.json
```

#### Proposed Gating Thresholds (from Research-2029)

The following thresholds are proposed for the 1.0.0 release pass:

1. **Synthetic PLCC Drop (`measure_quant_drop.py`)**:
   - **Static PTQ**: PLCC drop $\le 0.002$ (tightened from 0.01; empirical smoke achieved 0.00046).
   - **QAT**: PLCC drop $\le 0.001$ (tightened from 0.01; empirical smoke achieved 0.00037).
2. **Real Feature / Score Validation Gate**:
   - Mean absolute VMAF score delta across held-out clips: $\le 0.10$ VMAF points.
   - Maximum single-frame absolute delta: $\le 0.50$ VMAF points.
   - Held-out feature set PLCC: $\ge 0.990$.

---

## 8. Validation Gates and Model Cards

### 8.1 Numerical Validation Gates

Retrained models must pass their respective statistical gates:

- **FR Regressors (`fr_regressor_v1..v3`)**:
  Mean LOSO PLCC $\ge 0.95$ across folds (ADR-0168 / ADR-0302 / ADR-0309).
- **Ensemble Seeds (`fr_regressor_v2_ensemble`)**:
  Mean LOSO PLCC $\ge 0.95$ AND PLCC spread $\le 0.005$ across all 5 seeds (ADR-0303 / ADR-0309).
- **Tiny Regressors (`vmaf_tiny_v2..v4`)**:
  Minimum PLCC $\ge 0.990$ against the v1 teacher labels (`ai/scripts/validate_vmaf_tiny_v2.py`).
- **NR Metric (`nr_metric_v1`)**:
  Mean PLCC $\ge 0.85$, SROCC $\ge 0.80$, RMSE $\le 0.50$ on held-out MOS splits (ADR-0325).
- **Per-Shot Predictors (`model/predictor_*.onnx`)**:
  Mean LOSO PLCC $\ge 0.95$, spread $\le 0.005$ (`train_predictor_v2_realcorpus.py`).
- **Netflix Golden Preservation**:
  Execute `make test-netflix-golden` on the host. Must pass 271/271 tests.

### 8.2 Model Card Updates

Every retrained model must have its card updated under `docs/ai/models/<model>.md`
in compliance with the 5-point bar of [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md):

1. Plain-English functional summary.
2. Output range and qualitative interpretation.
3. Runnable usage example (CLI, C API, or Python).
4. Full provenance: trained against teacher `vmaf_v1.0.16_3d0h`, dataset composition, git SHA, license (`BSD-3-Clause-Plus-Patent`).
5. Known limitations (geometry bounds, color spaces, unsupported options).

---

## 9. Registry Update and ADR-1105 Ensemble Decision Point

### 9.1 Update `model/tiny/registry.json`

For every retrained model:

1. Update `sha256` to the new FP32 ONNX hash (64-character lowercase hex).
2. Set `quant_mode` to `"static"` or `"qat"`.
3. Update `int8_sha256` to the new INT8 ONNX hash.
4. Update `quant_accuracy_budget_plcc` (e.g. `0.002` for static PTQ, `0.001` for QAT).
5. Ensure sidecars have `"onnx_has_scaler": true` where standardisation runs in-graph.

### 9.2 ADR-1105 Ensemble Decision Point

If `train_fr_regressor_v2_ensemble_loso.py` passes the production gate
(`mean(PLCC) >= 0.95` and `spread <= 0.005` recorded in `runs/ensemble_v2_real/PROMOTE.json`):

1. Run seed export and registry patch:
   ```bash
   docker exec vmaf-dev-mcp env PYTHONPATH=/workspace/ai/src \
     /opt/vmaf-venv/bin/python /workspace/ai/scripts/export_ensemble_v2_seeds.py \
     --corpus /workspace/runs/phase_a/full_grid/per_frame_canonical6.jsonl \
     --promote-json /workspace/runs/ensemble_v2_real/PROMOTE.json \
     --update-registry
   ```
2. Remove `@pytest.mark.xfail(strict=True)` from
   `test_fr_regressor_v2_ensemble_seed_rows_are_production` in
   `python/test/model_registry_schema_test.py`.
3. Validate registry schema integrity:
   ```bash
   docker exec vmaf-dev-mcp /opt/vmaf-venv/bin/python /workspace/ai/scripts/validate_model_registry.py
   ```

*If the ensemble gate fails, retain `smoke: true` on the 5 seeds and leave the xfail marker intact.*

---

## 10. Rollback Strategy

If any post-retrain validation gate, quantisation drop threshold, or numerical
contract fails:

1. **Revert Model Files**:
   ```bash
   git checkout origin/master -- model/tiny/
   ```
2. **Revert Registry and Tests**:
   ```bash
   git checkout origin/master -- model/tiny/registry.json python/test/model_registry_schema_test.py
   ```
3. **Purge Run Staging**:
   ```bash
   rm -rf runs/shards/ runs/ensemble_v2_real/ runs/*.pt runs/*.json
   ```
4. **Continue Shipping Prior Baselines**:
   The existing FP32 models and dynamic-INT8 variants remain fully operational and
   continue shipping for 1.0.0.
   *(Note: The default model `vmaf_v1.0.16_3d0h` for classic VMAF calculations is unaffected
   by tiny-AI rollback; it operates independently under ADR-1169).*

---

## 11. Execution Timeline and Daily Driver Operations

### 11.1 Timeline Table (~130 h Total)

| Phase | Operation | Device / Backends | Wall-Clock Estimate | Basis & Source |
|---|---|---|---|---|
| **Phase 0** | Preconditions check & container rebuild | Host / Docker | ~0.5 h | Base image rebuild & toolchain verification |
| **Phase 1** | K150K re-smoke (≤5 clips) & clean | CUDA / CPU | ~0.5 h | Smoke verification & disk zero-leak validation |
| **Phase 2a** | K150K feature extraction (152,265 clips) | **CUDA** (RTX 4090) | **~105–110 h** | 0.36–0.40 clip/s across 8 worker processes |
| **Phase 2b** | Netflix, BVI-DVC, UGC, CHUG extraction | **CPU** (Zen 5, 32-th) | *~25–30 h* *(parallel)* | Runs concurrently with Phase 2a on CPU |
| **Phase 3** | Mixed-teacher check & parquet combination | CPU / PyArrow | ~1.0 h | Read and concatenate Parquet shards |
| **Phase 4** | FP32 model family retraining & export | CUDA / CPU | ~8–10 h | Multi-epoch MLP, LOSO ensemble & MOS heads |
| **Phase 5** | Static PTQ, QAT export & `check-ops` | CPU / PyTorch FX | ~3–4 h | Calibration runs, QDQ insertion, op allowlist |
| **Phase 6** | Quantisation drop gating (`measure_quant_drop`)| CPU / ORT | ~1.0 h | Synthetic & held-out error evaluation |
| **Phase 7** | Validation gates & model-card updates | Host / Python | ~2.0 h | Golden checks, PLCC confirmation, doc authoring |
| **Phase 8** | Registry update, Sigstore & ADR-1105 flip | Host | ~1.0 h | SHA-256 recalculation, schema validation |
| **Total** | **End-to-End One-Shot Retraining Pass** | **Workstation** | **~125–130 h** | Bounded by GPU K150K extraction + retrain pass |

### 11.2 What to Watch (Workstation as Daily Driver)

- **One Device Per Job (AGENTS.md §12)**:
  Never schedule sibling jobs on the RTX 4090 while K150K is extracting. The Zen 5
  CPU handles Netflix/BVI/UGC/CHUG extractions. The SYCL Arc A380 remains idle
  (`--no_sycl`) per maintainer decision D6.
- **Process Priority (Nice / Ionice)**:
  To maintain desktop responsiveness during the multi-day run, launch background
  docker commands with `nice` and `ionice`:
  ```bash
  nice -n 15 ionice -c 3 docker exec ...
  ```
- **GPU VRAM & Temperature Monitoring**:
  Monitor RTX 4090 temperature and memory headroom:
  ```bash
  watch -n 5 nvidia-smi
  ```
  Expected VRAM usage per worker: ~1.5–2.0 GB (~14–16 GB total across 8 workers).
- **Disk Headroom**:
  Ensure $\ge 50$ GB free space on the host volume mounting `/workspace/runs/` and
  $\ge 20$ GB on `/tmp/` for scratch YUV frames. Scratch YUVs are purged per-clip.
- **Pause and Resume**:
  K150K extraction is fully resumable through `.done` tracking files. To pause
  temporarily without killing state:
  ```bash
  kill -STOP <extract_k150k_pid>
  # To resume:
  kill -CONT <extract_k150k_pid>
  ```

---

## 12. Unverified Items

In accordance with project operating rules, the following items cannot be
verified against current live `master` state and are flagged:

1. **SYCL Feature Extraction Lane (UNVERIFIED)**:
   The SYCL backend is excluded from this runbook per maintainer decision D6.
   PR [#1307](https://github.com/VMAFx/vmafx/pull/1307) addresses v1 model crashes
   on Arc A380, but open drift rows (`T-SYCL-CAMBI-PARITY-DRIFT-2026-09-05` and
   `T-SYCL-MOTION2-CHECKERBOARD-DRIFT-2026-09-05`) prevent using SYCL for training data.
2. **`--vmaf-model` CLI Plumbing in `extract_k150k_features.py` (UNVERIFIED on master)**:
   The `--vmaf-model` argument and row-level provenance logic exist on branch
   `feat/ai-teacher-single-source` (PR [#1302](https://github.com/VMAFx/vmafx/pull/1302)),
   which is not yet merged to `master`. Verified on branch PR #1302.
3. **C Loader `.int8.onnx` Redirect (`vmaf --tiny-model`) (UNVERIFIED on master)**:
   As documented in Research-2029, `vmaf_use_tiny_model()` currently lacks the
   `.int8.onnx` redirect logic (tracked as open item in Epic #1242). Retrained
   models can be verified by explicit path invocation (`--tiny-model model.int8.onnx`).
