## CLI text golden pins: regenerate goldens, never hand-merge them (2026-11-13)

- The four goldens under `core/tools/test/golden/` conflict on any help-text
  change; resolve by regenerating the golden from the post-rebase binary,
  never by hand-merging the transcript.
- The `test_vmaf_per_shot.sh` hunk sits right after its
  `# 1. --help returns 0.` block; a conflict there keeps both neighbouring
  checks.
- **On sync**: an upstream help-text change regenerates the matching golden
  in the same pull request; `test_vmaf_cli_text_golden`,
  `test_vmaf_roi_usage`, `test_vmaf_vpl_usage` and `test_vmaf_per_shot`
  guard the transcripts (ADR-3543).
