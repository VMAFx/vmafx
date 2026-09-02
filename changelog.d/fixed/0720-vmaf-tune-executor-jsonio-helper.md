Made Phase-F `vmaf-tune` executor result JSONL portable strict JSON by
serializing failed-score `NaN` diagnostics as `null` while preserving the
in-memory result objects for callers.
