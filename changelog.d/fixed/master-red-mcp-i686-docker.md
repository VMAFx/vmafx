- MCP smoke: three `test_coverage_round6.py` tests that exercise ffprobe error-handling
  paths now stub `shutil.which` so they run on CI runners without ffprobe installed
  (previously raised `RuntimeError: ffprobe not on PATH` instead of testing the intended
  branch). Tests are not disabled or deleted — the `shutil.which` mock makes them
  self-contained.
- i686 no-asm build: `enable_avx512=true` + `enable_asm=false` no longer hard-errors at
  configure time. The guard is downgraded from `error()` to `warning()` + force-disable,
  matching the `core/AGENTS.md` invariant that this combination produces a warning, not
  an error. Explicitly passing `enable_avx512=enabled` (feature-flag style) still errors
  as expected.
- Docker smoke: the `RUN ldconfig` layer now writes an explicit
  `/etc/ld.so.conf.d/vmaf-local.conf` entry for `/usr/local/lib/x86_64-linux-gnu` before
  invoking `ldconfig`. The Ubuntu 26.04 CUDA base image (bumped in #876) does not include
  that arch-specific path in its default `ld.so.conf`, causing the installed vmaf binary
  to exit with a dynamic linker error (zero smoke-test stdout) in under 2 ms.
