- **`train_konvid.py` run-provenance sidecar** (ADR-0668 follow-up): the C2/C3
  KoNViD frame-trainer now emits a `train-konvid-manifest-v1` JSON sidecar
  alongside the PyTorch Lightning checkpoints. The sidecar records the full
  CLI argv, parsed args, input parquet paths, output checkpoint paths, and
  `run_provenance` block (entrypoint, git SHA, timestamp). Default path is
  `<output-c2>/train_konvid.manifest.json`; override with `--manifest-out`.
