Repair the `Build — Windows MinGW64 (CPU)` matrix leg. `test_public_api_score`'s
`test_vmaf_write_output` case hardcoded `/tmp/vmaf_test_output_XXXXXX` + `mkstemp(3)`;
MSYS2/MinGW64 inside the `windows-latest` runner does not expose a usable `/tmp`, so
the test failed with `mkstemp failed` and the master job was perpetually red. Factor
the temp-path setup into `make_temp_output_path()` (uses `GetTempPathA()` on Windows,
keeps `mkstemp` on POSIX), mirroring the precedent in `test_sidecar_parses`. See
ADR-0513.
