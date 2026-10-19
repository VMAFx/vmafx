<!-- markdownlint-disable MD013 MD060 -->
# ADR-2646: POSIX-only build parts stay off Windows, and preflight refuses an unguarded POSIX header

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: `build`, `ci`, `windows`, `preflight`

## Context

MSVC ships no `<unistd.h>`, `<dlfcn.h>`, `<poll.h>`, `<sys/socket.h>` or the
other POSIX-only headers. A C, C++ or CUDA file that includes one compiles on
every Linux and macOS lane and fails only on the required `Windows MSVC+CUDA` and
`Windows MSVC+SYCL` lanes. The local gates and the merge train build neither,
so the break first shows up on a pull request: on 2026-10-08 the Vulkan frame
import of the RC4 API stack added `<unistd.h>` to `core/src/cuda/import_vulkan.c`
for `dup()` and `close()`, and the CUDA backend is built on Windows.

The `msvcism` stage of `scripts/dev/preflight.sh`
([ADR-1234](1234-local-preflight-gate.md)) checks MSVC rejection classes
statically, without a compiler, and had no rule for this class. Run over the
whole tree, a header check found 32 includes in 9 groups. Most sit in files the
Windows build never compiles; three groups are compiled by an option a Windows
configure accepts:

- the embedded MCP server (`enable_mcp`, `core/src/mcp/`): Unix-domain and TCP
  sockets;
- the libFuzzer harnesses (`fuzz`, `core/test/fuzz/`): `<unistd.h>`;
- the `vmaf_vpl` tool (built with `enable_sycl` when VPL, VA-API and libva-drm
  are found): VA-API and `<unistd.h>`.

Two files are POSIX-only by design and no `meson.build` compiles them: the
vendored MATLAB MEX source
`compat/python-vmaf/matlab/strred/matlabPyrTools/MEX/innerProd.c`, and the
`LD_PRELOAD` fault-injection shim `ffmpeg-patches/test/fault_inject_libvmaf.c`.

## Decision

1. The MCP server, the libFuzzer harnesses and `vmaf_vpl` are POSIX-only build
   parts. Their Meson blocks carry `host_machine.system() != 'windows'`.
2. A Windows configure that asks for one of them fails with a message naming
   the POSIX dependency: `enable_mcp=true` in `core/src/meson.build`, `fuzz=true`
   in `core/test/meson.build`. No option requests `vmaf_vpl`; a Windows
   configure with `enable_sycl=true` prints that the tool is disabled because it
   is Linux only, as it already prints a disabled tool on Linux. Nothing is
   skipped silently.
3. The `msvcism` stage refuses a POSIX-only header in a changed source the
   Windows build compiles when the include sits outside every preprocessor
   conditional that names a platform. `scripts/dev/find-posix-only-headers.py`
   decides which sources the Windows build compiles from the `meson.build` files:
   a source named only inside a block kept off Windows (including the `subdir()`
   that reads its directory, a `foreach` over a list filled only off Windows, and
   a list kept empty off Linux) is not built there; a header counts through the
   sources that include it; a source no `meson.build` names counts as built.
4. The two design-POSIX files are named exceptions in
   `.config/lint-exceptions.d/msvcism-posix-headers.toml` (rule
   `msvcism-posix-headers`), each with a reason and an expiry
   ([ADR-1142](1142-whole-codebase-standards.md) exception form). No
   other file gets one.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Gate the three build parts off Windows, fail configure on an explicit request, except the two design-POSIX files (chosen) | The check reads the build the way the Windows lanes do; an explicit Windows request fails at configure with a reason; two exceptions, both outside the build | The scanner models a subset of Meson's control flow | Chosen |
| One named exception per finding file | No build change | About 20 exceptions that each restate "not built on Windows"; a new POSIX file under `core/src/mcp/` needs one more; a Windows `enable_mcp=true` still fails in the compiler | An unbounded exception list is the tier ADR-1142 retired |
| Scope the check to `core/src/` and gate the rest | Small scanner | Leaves `core/tools/`, `core/test/` and the CUDA sources outside the check; the 2026-10-08 break class reappears there | Partial coverage of a class that only the Windows lanes see |
| Port the MCP server, the harnesses and `vmaf_vpl` to Windows | Feature parity | Winsock and named-pipe transports, a VA-API replacement, Windows libFuzzer; no user asked for any of it | Out of RC scope; the gate keeps the door open for a later port |

## Consequences

- **Positive**: an unguarded POSIX header in a source the Windows build compiles
  fails locally, before a pull request reaches the MSVC lanes. A Windows
  configure that asks for a POSIX-only part fails at once with a reason instead
  of in the compiler.
- **Negative**: `find-posix-only-headers.py` understands `if` / `elif` / `else`,
  `foreach`, `subdir()` and the empty-list ternary; an unusual way to keep a
  source off Windows can produce a false finding, fixed in the scanner or with a
  named exception.
- **Neutral / follow-ups**: `scripts/ci/tests/test-preflight-msvcism.sh`, run by
  `make lint-sh`, plants an unguarded `<unistd.h>` in a source the Windows build
  compiles and checks that the stage refuses it, that a guarded include and a
  source kept off Windows pass, and that a scan that cannot run fails the stage.
  Master's stage before this change passed the planted include.

## References

- [ADR-1234](1234-local-preflight-gate.md): the local preflight gate and its
  `msvcism` stage.
- [ADR-1142](1142-whole-codebase-standards.md): every standard binds every
  file; exceptions name file, rule, reason and expiry.
- Source: `Q-266` (maintainer, 2026-10-08): "Yes, with a planted negative case run
  against the old stage".
- Source: `Q-269` (maintainer, 2026-10-08): "Option (b): mcp, fuzz and vpl meson
  blocks get a non-Windows gate that fails configure with a clear message when
  requested on Windows; only the MATLAB MEX file and the LD_PRELOAD shim get
  named, expiring exceptions".
