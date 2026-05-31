<!-- markdownlint-disable MD060 -->
# Ensemble training kit

Portable, one-command bundle for running the `fr_regressor_v2_ensemble_v1`
Phase-A corpus generation + 5-seed × 9-fold LOSO retrain + verdict
emission against an operator-supplied set of Netflix reference YUVs.

ADR: [ADR-0324](../../docs/adr/0324-ensemble-training-kit.md).
Parent design: [ADR-0303](../../docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md)
(production-flip gate definition), [ADR-0319](../../docs/adr/0319-ensemble-loso-trainer-real-impl.md)
(LOSO trainer real impl), [ADR-0321](../../docs/adr/0321-fr-regressor-v2-ensemble-full-prod-flip.md)
(full production flip + ONNX export script).

## I received a Google Drive bundle — what now?

The lead user ships **one zstd-compressed tarball** named
`ensemble-bundle-<ts>.tar.zst` plus its `.sha256` sidecar. The tarball
contains the kit + a `corpus/` tree of lossless-HEVC `.mkv` files
(~100 GiB transit-compressed; expands to ~229 GiB of bit-exact `.yuv`
references on extract). End-to-end runbook:

1. **Download** the `.tar.zst` and its `.sha256` from the shared drive
   folder. Use Google Drive Desktop or `rclone` — the browser's
   "Download all" sometimes silently drops files larger than 10 GB.
2. **Verify the tarball** before unpacking:

   ```bash
   sha256sum -c ensemble-bundle-<ts>.tar.zst.sha256
   ```

   If the line FAILs, redownload (don't proceed; the trainer needs
   bit-exact bytes).
3. **Untar** into an empty working directory:

   ```bash
   mkdir ensemble-run && cd ensemble-run
   tar --use-compress-program=unzstd -xf /path/to/ensemble-bundle-<ts>.tar.zst
   cd ensemble-training-kit
   ```

4. **Decode the corpus** back to raw YUV (one-time, ~30–60 min on a
   decent CPU; the script verifies every recovered YUV against the
   bundled manifest sha256, so you'll catch a bad download here even
   if step 2 passed):

   ```bash
   bash extract-corpus.sh --parallel 8
   ```

   On success it prints `verified: N / N YUVs match manifest.sha256`
   and (by default) deletes the intermediate `.mkv` files to free
   disk. Pass `--keep-mkv` if you want to retain them.
5. **Run the prereq check**:

   ```bash
   bash 01-prereqs.sh
   ```

6. **Run the pipeline**:

   ```bash
   bash run-full-pipeline.sh --ref-dir ./corpus
   ```

   On NVIDIA hardware this typically takes 4–10 hours wall-time
   depending on the GPU; on Apple Silicon / Intel Arc closer to a
   day. The script prints progress + a final verdict to stderr.
7. **Send the result tarball back** — exactly one file:

   ```text
   lawrence-ensemble-results-<ts>.tar.gz
   ```

   Upload it to the same Google Drive folder. See "How to send back
   the results" below for what's in the tarball and how the lead user
   uses it.

That's the whole loop — the kit emits one file, you upload one file,
the lead user takes it from there.

### Compressed corpus rationale

The lead user generates the tarball with
`prepare-gdrive-bundle.sh`, which encodes every reference YUV to
**lossless HEVC** (libx265 with `lossless=1`). Lossless HEVC gets
~55 % size reduction on natural content vs raw YUV; the
`extract-corpus.sh` step decodes back to the **bit-exact** YUV (the
manifest's per-file `yuv_sha256` proves this) before the trainer
ever sees the file. Alternative codecs (FFV1, AV1-lossless) are
selectable via `--codec` on the prep side; the extract side
auto-handles whichever container the bundle was built with.

## What this kit does

- Generates a Phase-A canonical-6 JSONL corpus from operator-supplied
  reference YUVs by encoding with NVENC at the standard CQ grid,
  decoding back to raw YUV, and scoring with libvmaf (CUDA backend).
- Runs the 5-seed × 9-fold LOSO retrain via the in-tree LOSO trainer,
  emitting one `loso_seed{0..4}.json` per seed.
- Applies the ADR-0303 two-part production-flip gate (`mean(PLCC) ≥ 0.95`
  AND `max(PLCC) − min(PLCC) ≤ 0.005`) and writes either `PROMOTE.json`
  or `HOLD.json`.
- On PROMOTE, exports per-seed ONNX members fitted on the FULL corpus
  (no held-out fold) and tars verdict + ONNX exports + manifest for
  return transport to the lead user.

## Prerequisites

| Requirement     | Expected                                                                              |
|-----------------|---------------------------------------------------------------------------------------|
| OS              | Linux (Ubuntu 22.04+ / Fedora 40+ / Arch / similar)                                   |
| GPU             | NVIDIA, ≥ 6 GB VRAM free, driver supporting CUDA toolkit ≥ 12.x                       |
| ffmpeg          | Built with `--enable-libnvenc` (`ffmpeg -encoders \| grep nvenc` shows `h264_nvenc`)  |
| libvmaf (CUDA)  | `core/build-cuda/tools/vmaf` — `meson setup build-cuda -Denable_cuda=true && ninja -C build-cuda` |
| Python          | ≥ 3.12 with `torch` (CUDA-enabled), `numpy`, `pandas`, `onnx`, `onnxruntime`, `scipy`, `scikit-learn` |
| Reference YUVs  | One per source, as raw `.yuv` files in a single directory                             |
| Free disk       | ≥ 5 GB for Phase-A intermediate mp4/yuv + ≥ 1 GB under the run output dir             |

The script `01-prereqs.sh` checks each of these and fails loudly with a
remediation hint if anything is missing. See "Troubleshooting" below.

QSV (Intel Arc / iHD) is **optional** — if `vainfo` does not surface
the iHD driver, the corpus generator silently skips QSV encoders and
runs NVENC-only. The model trains successfully on NVENC-only data;
the `encoder` one-hot collapses onto a single column.

The kit also runs on Intel Arc / Intel iGPU and macOS (Apple Silicon +
Intel) — see the **Multi-platform layout** section below for the
per-box recipe and the corpus-shard merge step.

## How to run

```bash
# One-command full pipeline (assumes prereqs are met; checks them first).
bash tools/ensemble-training-kit/run-full-pipeline.sh \
    --ref-dir /path/to/netflix/ref
```

Optional flags:

```bash
bash tools/ensemble-training-kit/run-full-pipeline.sh \
    --ref-dir   /path/to/netflix/ref \
    --encoders  h264_nvenc,hevc_nvenc \
    --cqs       19,25,31,37 \
    --out-dir   /path/to/run_output \
    --seeds     0,1,2,3,4
```

Defaults: `--encoders=h264_nvenc`, `--cqs=19,25,31,37`,
`--out-dir=$REPO_ROOT/runs/ensemble_v2_real`, `--seeds=0,1,2,3,4`.

The script exits with rc=0 on PROMOTE, rc=1 on HOLD, rc=2 on
prerequisite or input error.

### Step-by-step (manual)

The orchestrator is just five shell scripts chained together. Run them
individually if you need to retry a single stage:

```bash
bash tools/ensemble-training-kit/01-prereqs.sh
REF_DIR=/path/to/netflix/ref bash tools/ensemble-training-kit/02-generate-corpus.sh
bash tools/ensemble-training-kit/03-train-loso.sh
bash tools/ensemble-training-kit/04-validate.sh
bash tools/ensemble-training-kit/05-bundle-results.sh
```

Each step honours the same env vars (`REF_DIR`, `OUT_DIR`,
`CORPUS_JSONL`, `LIBVMAF_BIN`, `ENCODERS`, `CQS`).

## What you get back

After a successful run:

```text
$OUT_DIR/
├── PROMOTE.json (or HOLD.json)        # ADR-0303 verdict file
├── loso_seed0.json … loso_seed4.json  # per-seed LOSO artefacts
├── exports/                           # PROMOTE only — per-seed ONNX members
│   ├── fr_regressor_v2_ensemble_v1_seed0.onnx
│   ├── fr_regressor_v2_ensemble_v1_seed0.json
│   └── … (× 5 seeds)
├── logs/                              # per-seed stdout/stderr logs
└── manifest.json                      # sha256 + sizes of every artefact
```

The bundle script tars `$OUT_DIR/` whole into
`lawrence-ensemble-results-<ts>.tar.gz`. The verdict file's structure
is documented in [`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`](../../docs/ai/ensemble-v2-real-corpus-retrain-runbook.md).

### `PROMOTE.json` shape

```json
{
  "schema_version": 1,
  "verdict": "PROMOTE",
  "generated_at_utc": "2026-05-06T...",
  "seeds": [0, 1, 2, 3, 4],
  "gate": {
    "passed": true,
    "mean_plcc": 0.997,
    "plcc_spread": 0.001,
    ...
  },
  "corpus": { "sha256": "...", "yuv_count": 9, ... },
  "recommendation": "flip seeds smoke->false in model/tiny/registry.json (...)",
  "adr": "ADR-0309",
  "parent_adr": "ADR-0303"
}
```

## Troubleshooting

### `ffmpeg present but lacks h264_nvenc encoder`

Distro packages occasionally strip NVENC. Install ffmpeg from a build
that links the NVIDIA Video Codec SDK headers (`nv-codec-headers`),
or download a static build from `johnvansickle.com/ffmpeg/`.

Verify with `ffmpeg -encoders | grep h264_nvenc`.

### `libvmaf-CUDA binary not found`

Build it from the repo root:

```bash
meson setup build-cuda -Denable_cuda=true -Denable_sycl=false
ninja -C build-cuda
ls core/build-cuda/tools/vmaf
```

If meson cannot find nvcc, set `CUDA_HOME=/opt/cuda` (or wherever the
toolkit lives) and re-run setup. CUDA toolkit ≥ 12.0 is required.

### `torch.cuda.is_available() is False`

Re-install torch with the CUDA build matching your driver:

```bash
pip install --index-url https://download.pytorch.org/whl/cu124 torch
```

(Pick the channel matching your CUDA runtime: `cu121`, `cu124`, etc.)

### `GPU has only N MiB free`

Close other CUDA workloads, then re-run. The training pipeline needs
~6 GiB peak (libvmaf scoring shares the GPU with torch).

### Encode loop produces 0 rows

The `hw_encoder_corpus.py` source assumes 1920×1080@25fps `yuv420p` by
default; override with `WIDTH=`, `HEIGHT=`, `PIX_FMT=`, `FRAMERATE=`
before invoking step 02 if your YUVs differ.

### LOSO step reports `corpus has only N rows; need >=100`

Check `runs/phase_a/full_grid/per_frame_canonical6.jsonl` exists and
has the expected ~5640 rows for a 9-source × 4-CQ NVENC-only run. If
empty, step 02 silently skipped every source; re-run with the
`--encoders` / `--cqs` flags spelled correctly.

## How to send back the results

The lead user needs **only** the bundled tarball:

```text
lawrence-ensemble-results-<ts>.tar.gz
```

Untar to inspect contents; the manifest at `manifest.json` lists every
file with sha256 + size. On PROMOTE, the lead user's follow-up PR
re-checksums the per-seed ONNX exports and updates
`model/tiny/registry.json` to point at them.

Do **not** include the original `.yuv` reference files — they're not
part of the kit's output and are typically several GiB.

## Multi-platform layout

The kit runs on four host families. Each box generates its own
canonical-6 corpus shard; the lead user merges the shards via
`ai/scripts/merge_corpora.py` before training the production model.

The orchestrator auto-detects the platform and picks the right encoder
default; pass `--encoders` explicitly to override.

| Platform tag             | Auto-detect signal                | Default encoders                                  | libvmaf binary                       |
|--------------------------|-----------------------------------|---------------------------------------------------|--------------------------------------|
| `linux-x86_64-cuda`      | `nvidia-smi` exits 0              | `h264_nvenc,hevc_nvenc,av1_nvenc`                 | `binaries/linux-x86_64-cuda/vmaf`    |
| `linux-x86_64-sycl`      | `vainfo` shows iHD, no NVIDIA     | `h264_qsv,hevc_qsv,av1_qsv`                       | `binaries/linux-x86_64-sycl/vmaf`    |
| `linux-x86_64-vulkan`    | Linux without NVIDIA / iHD        | `libx264` (CPU baseline)                          | `binaries/linux-x86_64-vulkan/vmaf`  |
| `darwin-arm64-cpu`       | `uname -s` = Darwin, `-m` = arm64 | `h264_videotoolbox,hevc_videotoolbox`             | `binaries/darwin-arm64-cpu/vmaf`     |
| `darwin-x86_64-cpu`      | `uname -s` = Darwin, `-m` = x86_64| `h264_videotoolbox,hevc_videotoolbox`             | `binaries/darwin-x86_64-cpu/vmaf`    |

Both NVIDIA + iHD signals firing on the same box (e.g. a workstation
with an NVIDIA dGPU and an Intel iGPU) tags the platform as
`linux-x86_64-cuda` but populates the encoder default with **both**
families so the corpus picks up NVENC and QSV rows in one pass.

### Per-box recipe

#### NVIDIA CUDA Linux box

```bash
bash tools/ensemble-training-kit/build-libvmaf-binaries.sh --platform linux-x86_64-cuda
bash tools/ensemble-training-kit/run-full-pipeline.sh --ref-dir /path/to/netflix/ref
```

Encoders auto-detected: `h264_nvenc,hevc_nvenc,av1_nvenc`. Encodes ALL
9 Netflix sources × 4 CQ values × 3 encoder lanes; the trainer's
`encoder` one-hot lights up the NVENC columns.

#### Intel Arc 310 Linux box

```bash
bash tools/ensemble-training-kit/build-libvmaf-binaries.sh --platform linux-x86_64-sycl
bash tools/ensemble-training-kit/run-full-pipeline.sh --ref-dir /path/to/netflix/ref
```

Encoders auto-detected: `h264_qsv,hevc_qsv,av1_qsv`. Same source set as
NVIDIA box; rows tagged with `encoder=h264_qsv` etc. so the merged
corpus carries one-hot rows for both families. AV1-QSV requires Arc
A-series or newer; if Arc 310 lacks AV1 the corpus generator skips that
encoder lane silently.

#### Intel iGPU Linux box

Same recipe as Intel Arc — the iGPU also exposes the iHD VA-API driver
when the i965 fallback is not in use. Override `--encoders` to
`h264_qsv,hevc_qsv` if AV1-QSV is unavailable on the iGPU generation
(11th-gen Tiger Lake and earlier).

#### macOS (Apple Silicon or Intel)

```bash
bash tools/ensemble-training-kit/build-libvmaf-binaries.sh --platform darwin-arm64-cpu
# or --platform darwin-x86_64-cpu on Intel Macs
bash tools/ensemble-training-kit/run-full-pipeline.sh --ref-dir /path/to/netflix/ref
```

Encoders auto-detected: `h264_videotoolbox,hevc_videotoolbox`. AV1 is
**not** available on Apple Silicon hardware as of 2026 — the kit
omits `av1_videotoolbox` from the default list. The CQ grid maps onto
VideoToolbox's `-q:v` axis via `q = clamp(100 - 2*cq, 1, 100)` so the
standard `--cqs 19,25,31,37` produces sensible quality points on the
VT scale.

### Merging per-box shards

After every box produces its
`runs/phase_a/full_grid/per_frame_canonical6.jsonl`, gather the JSONLs
on the lead user's box and merge:

```bash
python3 ai/scripts/merge_corpora.py \
    --inputs runs/phase_a/nvidia_box.jsonl \
             runs/phase_a/intel_arc_box.jsonl \
             runs/phase_a/intel_igpu_box.jsonl \
             runs/phase_a/macbook.jsonl \
    --output runs/phase_a/multi_platform_corpus.jsonl
```

The merger validates each row carries the canonical-6 schema and
de-duplicates by `src_sha256`. Re-run the LOSO trainer with
`CORPUS_JSONL=runs/phase_a/multi_platform_corpus.jsonl` to retrain on
the multi-encoder pool — encoder vocab v3 (16 slots, ADR-0302) lights
up NVENC + QSV + VAAPI + VideoToolbox columns simultaneously.

## Generating a portable bundle (lead user only)

For shipping the kit to a collaborator who doesn't have the vmaf fork
checked out:

```bash
bash tools/ensemble-training-kit/make-distribution-tarball.sh \
    /tmp/vmaf-ensemble-training-kit-$(date -u +%Y%m%d).tar.gz
```

Output is a self-contained tarball (~ < 50 MiB) with the kit + every
in-repo script it invokes + the runbook, no model weights, no test
data, no build artefacts. The collaborator extracts and treats the
extracted directory as `$REPO_ROOT`.
