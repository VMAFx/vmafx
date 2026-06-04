<!-- markdownlint-disable MD060 -->
# `compat/python-vmaf/workspace/` — Python harness workspace

Scratch tree used by the **Python training / evaluation harness** (i.e.
`python/vmaf/script/run_*.py` and friends). Not touched by libvmaf, the
`vmaf` CLI, or the SYCL / CUDA / HIP backends.

## Where is it?

| Fork                     | Location                                     |
| ------------------------ | -------------------------------------------- |
| Upstream Netflix/vmaf    | `workspace/` (at repo root)                  |
| This fork (Lusoris)      | `compat/python-vmaf/workspace/` (next to its user)  |

The move is motivated by CLAUDE.md §1 — the repo root is reserved for surfaces a
consumer actually builds against (`core/`, `ai/`, `mcp-server/`, `tools/`,
`docs/`, `model/`, `testdata/`). Everything else is pushed down into the subtree
that owns it.

Override the location at runtime with the `VMAF_WORKSPACE` environment variable
— useful for CI caches, read-only checkouts, or running training on a dataset
mount:

```bash
VMAF_WORKSPACE=/mnt/big_nvme/vmaf python -m vmaf.script.run_testing VMAF …
```

The resolution lives in [`compat/python-vmaf/config.py`](../../compat/python-vmaf/config.py)
as the module-level `WORKSPACE` constant; `VmafConfig.workspace_path()`,
`workdir_path()`, `encode_path()`, `encode_store_path()`, and
`file_result_store_path()` all derive from it.

## Subdirectory contract

Each subtree is created on demand by `os.makedirs(..., exist_ok=True)`. They
ship as empty `.gitignore` placeholders so the layout is discoverable, but the
harness works fine if you delete them entirely.

| Subdir               | Written by                                 | What's in it                                                                       |
| -------------------- | ------------------------------------------ | ---------------------------------------------------------------------------------- |
| `dataset/`           | `script/run_*` dataset loaders             | YUV / MP4 fixtures referenced by `resource/example/*_dataset.py` recipes.          |
| `encode/`            | `script/run_encode.py`                      | Encoder output for distortion-pair generation.                                     |
| `model/`             | `script/run_vmaf_training.py`               | Trained `.pkl` SVM / libsvmnusvr models (classic — NOT the fork's ONNX tiny models). |
| `model_param/`       | custom runs                                 | Pickled model hyperparameter sets.                                                 |
| `output/`            | any run with `--cache-result`               | Per-run pickled feature caches / plots.                                            |
| `result_store_dir/`  | `FileResultStore`, `EncodeResultStore`      | Persistent feature-extraction and encode caches so re-runs are fast.               |
| `checkpoints_dir/`   | training harness checkpointing              | Intermediate model checkpoints during long training runs.                          |
| `workdir/`           | `VmafConfig.workdir_path()`                 | Per-run UUID scratch — `ffmpeg` decode temp, preresampling pipes, intermediate YUVs. Always safe to `rm -rf`. |

## Relation to other model surfaces

| Surface                  | Where trained models live                     | Runtime                                                   |
| ------------------------ | --------------------------------------------- | --------------------------------------------------------- |
| Classic Netflix SVM      | `compat/python-vmaf/workspace/model/*.pkl`    | Read by the classic Python harness.                       |
| Shipped `vmaf_v0.6.1`    | [`model/`](../../model/) (JSON/pkl)           | Read by libvmaf via `--model path=...`.                   |
| Fork Tiny-AI (C1/C2/C3)  | [`model/tiny/*.onnx`](../../model/tiny/)      | Read by core/src/dnn/ via ONNX Runtime — see [docs/ai/](../ai/overview.md). |

**If you are adding a new fork feature** (a SIMD path, a GPU backend, a
metric), you almost certainly do not need this directory.

## When to wipe it

Always safe:

```bash
rm -rf python/vmaf/workspace/workdir/*    # per-run scratch, regenerated on next run
rm -rf python/vmaf/workspace/output/*     # per-run artefacts
```

Destroys caches (will re-extract features on next run — slow):

```bash
rm -rf python/vmaf/workspace/result_store_dir/
```

Destroys trained classic SVM models (can't be recovered without retraining):

```bash
rm -rf python/vmaf/workspace/model/          # careful
rm -rf python/vmaf/workspace/checkpoints_dir/
```
