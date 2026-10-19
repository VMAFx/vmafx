- **`vmaf-tune` subcommands declare their shared flags through one helper
  each.** `--src`, `--encoder`, `--pix-fmt`, `--framerate`, the CRF bounds,
  the binary paths and the frame dimensions are added by helpers in
  `tools/vmaf-tune/src/vmaftune/cli.py` instead of being repeated per
  subcommand; every flag's name, default, type, choices and help text is
  unchanged.
