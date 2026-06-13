- **fix(mcp)**: wrap `json.loads(output.read_text())` in `_run_vmaf_score` with
  a `JSONDecodeError` guard — partial/empty vmaf JSON (disk-full, OOM kill) now
  surfaces a clear `RuntimeError` instead of a raw traceback (ADR-1010).
- **fix(mcp)**: wrap `json.loads(result.stdout)` in `_ffprobe_geometry` with a
  `JSONDecodeError` guard — audio-only or corrupt container inputs now raise a
  descriptive `RuntimeError` instead of crashing `vmaf_score_encoded` (ADR-1010).
