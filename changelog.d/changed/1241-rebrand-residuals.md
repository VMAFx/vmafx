- **chore(rebrand):** Scrub the last live pre-rebrand identifiers left over
  from epic #1241. The root tooling distribution is now `vmafx-tooling` (was
  `vmaf-fork-tooling`; never published, no install path changes),
  `ai/scripts/export_transnet_v2.py` stamps
  `producer_name=vmafx-transnet-v2-export` on future exports,
  `ai/scripts/fetch_konvid_1k.py` identifies itself as
  `vmafx/fetch_konvid_1k.py`, the `/refresh-ffmpeg-patches` skill's default
  work branch is `vmafx-patches`, and package descriptions, MCP / dev-llm
  docstrings and prompts, and the `.claude/agents/` reviewer prompts say
  "VMAFx fork" instead of "Lusoris VMAF fork". The self-hosted-runner guide's
  `journalctl` unit name now matches the `VMAFx/vmafx` registration URL.
  Unchanged on purpose: the `libvmaf.so` ABI / soname, the `libvmaf` ffmpeg
  filter name, the version scheme, the `lusoris.*` ONNX metadata keys (a
  model-sidecar contract), and historical "formerly `lusoris/vmaf`" notes.
