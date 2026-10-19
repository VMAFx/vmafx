## The `vmafx` filter's pairing contract (2026-10-07)
## The `vmafx` filter's pairing contract (`rc4/api-wp15-ffmpeg-audit`)

RC4 WP15 ([ADR-2165](adr/2165-vmafx-filter-pairing-contract.md)) changes
`ffmpeg-patches/src/vf_vmafx.c` (the source of patch `0021`, refreshed with
`python3 scripts/ci/ffmpeg_patch_stack.py --refresh`), the `contract`
subcommand of `ffmpeg-patches/test/vmafx_filter_check.py` and the section
"Pairing the two inputs" of `docs/usage/ffmpeg.md`.

Invariants a rebase keeps:

- `vmafx_preinit()` and `vmafx_tune_preinit()` wrap the generated
  `*_framesync_preinit()` and set `eof_action=pass` and `repeatlast=0`; FFmpeg
  applies user options after `preinit`, so a user's `eof_action=repeat:repeatlast=1`
  still wins. An upstream change to `FRAMESYNC_AUXILIARY_FUNCS` keeps the wrapper.
- A main frame without a reference frame goes through `send_or_hold()` with
  `VMAFX_NOT_SCORED`, so it stays in order behind frames `metadata=1` holds;
  the unscored count is logged at uninit.
- `finish_stream()` and `uninit()` call `vmafx_flush()` only when a pair was
  submitted (`frame_cnt`), and `write_report()` writes nothing otherwise.
- `report_path_check()` runs at `init()`; the library still writes the report.

No score, public C API or option change beyond the framesync defaults. No
rebase impact on the `libvmaf*` patches.
