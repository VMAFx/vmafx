# ROCm 26.04 pin restoration — 2026-09-08

Commit `56756919666f69a3e4571596c56986a230623846` moved the ROCm sources to
Ubuntu 26.04 under ADR-1231. Commit
`53b970ec71b64fc14a53f942ea90d9cdd5af79f1` reverted the pins to ADR-1225's
older 24.04 image without updating the 26.04 documentation or recording a
new compatibility failure. This restores the already selected 26.04 image.

AMD's [published tag](https://hub.docker.com/r/rocm/dev-ubuntu-26.04/tags)
and a fresh registry manifest lookup both identify the released
`rocm/dev-ubuntu-26.04:10.0.0-full` as
`sha256:8ebc02ee1aefb6b7c0e952b4088542133bad8805d19730db92cc61be673421d1`.
The locally cached image resolves to that same digest and reports Ubuntu
26.04. HIP reports component version 7.15.26333; this does not change the
ROCm release version of 10.0.0.

## Compatibility checks

The vendor image compiles and links a small HIP kernel for `gfx1036` without
a GPU. A separate smoke image uses the exact eight ROCm `COPY` instructions
from `docker/Dockerfile.node` and the configured digest-pinned
`RELEASE_RUNTIME_CC` Debian 13 base. Its dynamic loader resolves
`libamdhip64.so` and every transitive dependency, including
`librocprofiler-register`, `libamd_comgr`, LLVM and the bundled system
libraries, with no unresolved library or symbol-version error.

The existing `rocm-src` stage claimed to gate pruning with a compiler smoke
test, but invoked only `hipconfig --version`. It now compiles/links the kernel
and runs its host-only entry point after pruning, with `/opt/rocm/lib` on
`LD_LIBRARY_PATH` as in the consuming stages. The first run exposed the
missing library search path in this new smoke command; setting the existing
runtime path made the same pruned SDK pass. No GPU device is mounted,
and the kernel is not launched. This is SDK and loader validation, not a new
claim of GPU numerical parity or a full release-image build.

Hadolint also rejected the existing Meson-wheel mount tokens split across
lines in `Dockerfile.production-gpu`. Keeping each mount token on one line
preserves its value and lets all three touched Dockerfiles pass the parser
and lint checks.

## Reproduction

```bash
docker buildx imagetools inspect rocm/dev-ubuntu-26.04:10.0.0-full
make base-images-sync
docker build --network none --target rocm-src -f dev/Containerfile .
```

The exact runtime smoke Dockerfile, registry/image identities, commands and
raw logs are retained in `.workingdir2/evidence/rocm-2604-restore-20260908/`.
The smoke Dockerfile copies the production recipe rather than a guessed
subset. Preserve the relative library layout and the compiler-after-pruning
check when changing the pin. No new alternatives or policy: restore the
ADR-1231 choice and test the compatibility it already requires.

User request: "24.04? O.o why not 26.04?"
