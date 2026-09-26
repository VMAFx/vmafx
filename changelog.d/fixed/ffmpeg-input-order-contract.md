- **ffmpeg:** Restored the exact `AV_LOG_INFO` warning that VMAF filter pad 0
  is distorted and pad 1 is reference, corrected the user-facing commands that
  violated that semantic order, and added a fail-closed Markdown contract with
  mutation tests in Make, pre-commit, and both FFmpeg patch-stack CI jobs.
