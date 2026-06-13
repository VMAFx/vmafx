`vmaf --help` now exits 0 (previously exited 1 because `--help` was not a
registered getopt option and fell through to the "reference required" error
path). Help text is now printed to stdout rather than stderr when invoked via
`--help`. `vmafx-server --help` and `vmafx-node --help` also exit 0.
