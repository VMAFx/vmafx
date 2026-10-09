---
paths:
  - ffmpeg-patches/test/build-and-run.sh
  - scripts/ci/upstream-consumer-lib.sh
  - scripts/ci/test_ffmpeg_patch_smoke_safety.py
invariant: MSVC FFmpeg check runs smoke script with FFMPEG_TOOLCHAIN=msvc, -MD, VMAF_SCORE_CHECK=1 against static vmaf.lib install.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# MSVC FFmpeg check (ADR-2783)

Required check `FFmpeg Windows MSVC` (`ffmpeg-msvc-work` / `ffmpeg-msvc-gate`
in `ffmpeg-integration.yml`) = static MSVC install (`vmaf.lib`, `vmafx.lib`,
ADR-2752) + `ffmpeg-patches/test/build-and-run.sh` in MSYS2 UCRT64 shell,
`path-type: inherit`. No second series replay in workflow: smoke script
owns checkout, `git am --3way` of whole series, configure, build, FATE subset,
warning gate, option probes, score check.

- `FFMPEG_TOOLCHAIN=msvc` -> `--toolchain=msvc --extra-cflags=-MD
  --extra-cxxflags=-MD`. `-MD` = Meson release default CRT (`b_vscrt`
  `from_buildtype`). Changing either side alone mixes runtimes; warning gate
  catches `LNK4098`. Never add `-Db_vscrt` to leg without matching FFmpeg
  flag.
- Settings checked before clone: `FFMPEG_TOOLCHAIN` (empty | msvc),
  `FFMPEG_JOBS` (1-999), `SMOKE_FATE` (0|1, skip printed), `VMAF_SCORE_CHECK`
  (0|1, needs `VMAF_PREFIX`).
- Score check reuses `upstream-consumer-lib.sh`: `uc_ffmpeg_graph` (one
  definition, also `upstream-ffmpeg-compat.sh`), `uc_cli_scores`,
  `uc_compare` (exact text). Log path stays relative: MSYS2 never converts
  absolute POSIX path inside filter graph for native `ffmpeg.exe`.
- `git config --global core.autocrlf false` + `core.eol lf` precede
  checkout: bash reads `build-config.env` (`text=auto`) and scripts;
  autocrlf=false alone still writes native CRLF. Smoke script also strips CR
  per `series.txt` line (`test_crlf_series_applies`).
- Warning gate under msvc = `msvc_findings`: every `warning LNK*` + every
  `file(line[,col]): warning C*` whose path (`\`->`/`, `.\` dropped, suffix
  match on `/`) and line lie on `+` side of `git diff -U0 $series_base HEAD`.
  FFmpeg's own ~400 cl warnings + D9024 stay out, also in files series edits
  (0019 -> `libavcodec/vlc.c`, C4334 on untouched lines). Other toolchains:
  every warning fails. Never widen back to blanket grep or file scope under
  msvc, never narrow to allowlist of codes or files.
- MSYS2 `git` package required: Git for Windows gets `/dev/null` and POSIX
  paths from smoke script.
- `test_ffmpeg_patch_smoke_safety.py` covers every setting, score match,
  last-digit mismatch and both warning gates (FFmpeg-own warnings pass, series
  file under three path forms + LNK4098 fail); `test_ffmpeg_patch_workflow_contract.py` pins job.
