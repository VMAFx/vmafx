- Add `VMAF_<NAME>_DIR` env-var overrides to every `ai/scripts/*.py` corpus
  path constant (ADR-0547). Maintainer defaults unchanged; container and
  non-maintainer operators can now point scripts at any layout with a single
  `export VMAF_CHUG_DIR=/path/...` instead of editing the script. Env vars:
  `VMAF_CHUG_DIR`, `VMAF_NETFLIX_CORPUS_DIR`, `VMAF_KONVID_1K_DIR`,
  `VMAF_KONVID_150K_DIR`, `VMAF_LSVQ_DIR`, `VMAF_LIVE_VQC_DIR`,
  `VMAF_YOUTUBE_UGC_DIR`, `VMAF_WATERLOO_IVC_DIR`, `VMAF_BVI_DVC_RAW_DIR`.
  Documented at `docs/ai/scripts-env-vars.md`.
- Add `*.bak` and `*.orig` to `.gitignore`; delete untracked 142 KB
  `tools/vmaf-tune/src/vmaftune/cli.py.bak` editor backup that was
  polluting `rg --type py` audit sweeps.
