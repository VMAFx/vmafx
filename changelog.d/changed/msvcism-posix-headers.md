- **POSIX-only build parts stay off Windows, and `preflight.sh --stage msvcism`
  refuses an unguarded POSIX header
  ([ADR-2646](docs/adr/2646-posix-only-build-options.md)).** On Windows,
  `-Denable_mcp=true` and `-Dfuzz=true` now stop configure with an error that
  names the POSIX dependency instead of failing in the compiler, and the
  `vmaf_vpl` tool is not looked for (configure prints why). The `msvcism` stage
  fails on a `<unistd.h>`, `<dlfcn.h>`, `<sys/socket.h>` or other POSIX-only
  include outside a platform conditional in a source the Windows build compiles
  (`scripts/dev/find-posix-only-headers.py`), and fails when that scan cannot run.
