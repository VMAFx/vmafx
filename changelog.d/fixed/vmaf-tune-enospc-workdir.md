`vmaf-tune compare` / `ladder` / `tune-per-shot`: fix rc=228 (ENOSPC) on
large sources by adding a disk-space preflight before the reference YUV
decode. Adds `VMAFTUNE_WORKDIR` env var and `--workdir PATH` CLI flag to
route scratch I/O to a larger volume; the dev container pre-sets
`VMAFTUNE_WORKDIR=/probes/vmaftune-work` (435 GB bind-mount). ADR-0549.
