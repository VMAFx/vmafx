- **CUDA toolkit pin bumped to 13.3** (`dev/Containerfile`):
  `cuda-toolkit-13-2` → `cuda-toolkit-13-3` (13.3.0-1 from NVIDIA ubuntu2404
  apt repo, GA 2026-05-28). Minimum host driver updated to R610.43.02 (Linux
  x86_64). Blackwell `sm_103` gencode row added to the gencode coverage table.
  CI GitHub Actions Jimver pin remains on `13.2.0` pending action-level support
  for 13.3 (tracked follow-up). NVCR image bump also deferred (no `13.3.x-devel`
  tag published yet). [ADR-0738](../docs/adr/0738-bump-cuda-133-r610-local.md).
