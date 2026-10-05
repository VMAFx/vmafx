- **`ai/train/qat.py` and the KonViD trainer test are at the HISS standard.**
  `run_qat()` was one function of 147 lines; it keeps its name and keyword
  signature and now delegates the fine-tune phases, the weight transfer, the
  ONNX export and the quantisation to helpers (its argument documentation moved
  to the module docstring). The CHUG display-profile entrypoint test builds its
  fixtures in two helpers and keeps every assertion. No behaviour change; the
  HISS baseline loses the two rows.
