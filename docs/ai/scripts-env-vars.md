# `ai/scripts/` corpus-path environment variables

Every corpus-ingestion / training script under `ai/scripts/` defaults to
the maintainer's local `.workingdir2/<corpus>/` directory. That path is
gitignored and only exists on the maintainer's machine, so on every
other host (including the [`vmaf-dev-mcp` container](../development/dev-mcp.md))
the first run fails with `FileNotFoundError`.

Per [ADR-0546](../adr/0546-audit-cleanup-bundle.md) each script accepts
an env-var override layered on top of the default. Set the env var and
the script picks up your local layout without any CLI edit; leave it
unset and the maintainer's defaults still apply.

## Overrides

| Script(s) | Env var | Default |
| --- | --- | --- |
| `chug_to_corpus_jsonl.py`, `chug_extract_features.py`, `train_chug_hdr_mos_head.py` (input shards) | `VMAF_CHUG_DIR` | `<repo>/.corpus/chug` |
| `train_chug_hdr_mos_head.py` (local model outputs) | `VMAF_CHUG_OUTPUT_DIR` | `<repo>/.workingdir2/chug` |
| `konvid_1k_to_corpus_jsonl.py`, `konvid_to_full_features.py`, `train_konvid_mos_head.py` (1k input) | `VMAF_KONVID_1K_DIR` | `<repo>/.workingdir2/konvid-1k`; full-feature extraction falls back to `$VMAF_DATA_ROOT/konvid-1k` when unset |
| `konvid_150k_to_corpus_jsonl.py`, `extract_k150k_features.py`, `train_konvid_mos_head.py` (150k input), `train_predictor_v2_realcorpus.py` | `VMAF_KONVID_150K_DIR` | `<repo>/.workingdir2/konvid-150k` |
| `lsvq_to_corpus_jsonl.py` | `VMAF_LSVQ_DIR` | `<repo>/.workingdir2/lsvq` |
| `live_vqc_to_corpus_jsonl.py` | `VMAF_LIVE_VQC_DIR` | `<repo>/.workingdir2/live-vqc` |
| `youtube_ugc_to_corpus_jsonl.py` | `VMAF_YOUTUBE_UGC_DIR` | `<repo>/.workingdir2/youtube-ugc` |
| `waterloo_ivc_to_corpus_jsonl.py` | `VMAF_WATERLOO_IVC_DIR` | `<repo>/.workingdir2/waterloo-ivc-4k` |
| `extract_full_features.py`, `eval_loso_mlp_small.py`, `eval_loso_3arch.py`, `validate_ensemble_seeds.py`, `train_predictor_v2_realcorpus.py` | `VMAF_NETFLIX_CORPUS_DIR` | `<repo>/.workingdir2/netflix` |
| `train_predictor_v2_realcorpus.py` | `VMAF_BVI_DVC_RAW_DIR` | `<repo>/.workingdir2/bvi-dvc-raw` |
| `bvi_dvc_to_full_features.py` | `VMAF_BVI_DVC_ZIP` | `<repo>/.workingdir2/BVI-DVC Part 1.zip` |

## Usage examples

```bash
# Inside the dev-mcp container with corpora bind-mounted under /workspace
export VMAF_CHUG_DIR=/workspace/chug
export VMAF_KONVID_150K_DIR=/workspace/konvid-150k
export VMAF_NETFLIX_CORPUS_DIR=/workspace/netflix

python ai/scripts/chug_extract_features.py            # picks up /workspace/chug
python ai/scripts/train_chug_hdr_mos_head.py          # picks up /workspace/chug
python ai/scripts/train_konvid_mos_head.py            # picks up /workspace/konvid-150k
python ai/scripts/extract_full_features.py            # picks up /workspace/netflix
python ai/scripts/konvid_to_full_features.py          # picks up /workspace/konvid-1k
```

The env-var override does not change the per-argument flags. Every
script still accepts an explicit `--data-root <path>` / `--clips-dir
<path>` / `--scores <path>` / etc. that takes precedence over both the
env var and the default. The env var sets a new *default* that the
operator can still override per-invocation on the CLI.

## Why this exists

The audit pass that produced [ADR-0546](../adr/0546-audit-cleanup-bundle.md)
flagged "heavy `.workingdir2/` defaults across 15+ scripts" as a
recurring friction point: anyone trying to reproduce the maintainer's
training runs in the container or on their own machine had to either
populate `.workingdir2/` symlinks or hand-edit each script. The env-var
layer is strictly additive — maintainer workflow is unchanged; everyone
else gains a one-line override.
