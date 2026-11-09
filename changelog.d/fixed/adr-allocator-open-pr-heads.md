- **ADR allocator scans open pull request heads to avoid fork PR collisions.**
  In claim mode, `scripts/adr/next-free.sh` queries `gh` for open pull request
  head commit SHAs and scans their `docs/adr` trees alongside origin branches.
  Contributors with a fork as origin can set `VMAF_ADR_PR_REPO=VMAFx/vmafx`.
  When `gh` is missing or fails, it warns on stderr and scans origin branches
  only.
