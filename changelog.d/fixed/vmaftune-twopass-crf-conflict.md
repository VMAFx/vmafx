- Fixed conflicting rate-control flags in `libx264` two-pass encoding (`T-VMAFTUNE-TWOPASS-CRF-INVALID-2026-08-30`).
  In two-pass mode (`pass_number != 0`), `libx264` requires bitrate mode rather than CRF
  (FFmpeg exits with status 187: `CRF/CQP is incompatible with 2pass.`). Both Go (`pkg/codecadapter`,
  `pkg/ffencode`, `pkg/corpus`) and Python (`vmaftune.codec_adapters.x264`, `vmaftune.encode`) now omit `-crf`
  when building multi-pass command lines, permitting clean two-pass execution with bitrate targets
  passed in extra parameters.
