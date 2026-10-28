## The `vmafx` filter check runs in CI (2026-10-28)

`.github/workflows/ffmpeg-patch-stack.yml` gains the job `filter` (name
`FFmpeg vmafx Filter`, listed in `.github/workflows/required-aggregator.yml`).
It builds libvmafx, then FFmpeg with the patch series and `--enable-libvmafx`,
and runs `ffmpeg-patches/test/vmafx_filter_check.py` `parity`, `windows`,
`contract` and `provenance` against the CLI of the same build.

Invariants a rebase keeps:

- The job stays required and keeps all four checks; a renamed subcommand of
  `vmafx_filter_check.py` changes the loop in the same PR.
- Exit 77 (missing fixture) fails the job; the fetch step brings the pair.
- `scripts/ci/test_ffmpeg_patch_workflow_contract.py`
  (`test_vmafx_filter_job_*`) guards the job, with planted gaps.
