<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3532: Preserve trusted executable launch links

- **Status**: Accepted
- **Date**: 2026-11-11
- **Deciders**: Repository maintainers
- **Tags**: python, security, testing

## Context

The bounded process runner checks the resolved executable against its allowlist, but a virtual environment also depends on the path passed to the operating system at launch. The macOS framework interpreter stub sets `__PYVENV_LAUNCHER__` from `_NSGetExecutablePath` in `Mac/Tools/pythonw.c`. Executing the resolved interpreter loses the virtual environment even when `argv[0]` retains its link name. The master Build run 42940218976 and Builds run 42940218931 failed in `python/test/tools_test.py` with a missing `numpy` module after resolved-file launch. The Linux interpreter's `Modules/getpath.py` uses `argv[0]`, so Linux results did not expose that regression.

Preserving the named launch path introduces another requirement: another user must not be able to retarget any followed symbolic link between validation and execution. The same rule applies to the byte-identical runner shipped in the tester package. Bounded traversal and fail-closed validation follow [principles.md](../principles.md).

## Decision

Launch the named absolute path on POSIX only after checking every symbolic link followed in kernel traversal order, including links introduced by directory components and relative or absolute link targets. Require each link's `lstat` owner and its already resolved containing directory's owner to be the current user or root; refuse group- or other-writable containing directories unless sticky. Fail with `CommandValidationError` identifying the offending link, without switching to resolved-file launch. Bound traversal with `MAX_EXECUTABLE_LINK_HOPS = 40` (Linux `MAXSYMLINKS`) and `MAX_EXECUTABLE_WALK_STEPS = 4096`, naming the exceeded bound in diagnostics. Keep allowlist and runnable-file checks on the resolved file. Platforms without `os.getuid`, including Windows, launch the resolved file; Windows virtual environments use real interpreter files.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Launch the resolved file | Removes symbolic-link retargeting from the launch path | Loses macOS virtual-environment discovery; master runs 42940218976 and 42940218931 failed | Refused for POSIX; retained on platforms without `os.getuid` |
| Set `executable` to the resolved file and retain the link as `argv[0]` | Preserves the argument name used by Linux discovery | Same previous implementation and same macOS failures: the stub observes the executed path | Does not preserve macOS virtual environments |
| Check trusted links, then launch the named path | Preserves virtual environments and rejects links another user could retarget | Requires bounded traversal and ownership checks; ancestor-directory gap remains | Chosen |

## Consequences

- **Positive**: Trusted virtual-environment interpreter links and multi-call executable names survive launch. Tests cover sticky directories, untrusted owners and modes, intermediate links, allowlist refusal, traversal order, and both bounds.
- **Negative**: Executables reached through untrusted links fail closed. The check assumes root and the current user are trusted and uses POSIX ownership and mode bits.
- **Neutral / follow-ups**: Ancestor directories above each link's containing directory are not checked. An ancestor replacement can still change path resolution, the same residual risk as a plain absolute-path launch. These checks do not provide an atomic executable handle or protect against the current user or root changing the path. The walker is a bounded component walk with no recursion. The runner copies remain byte-identical.

## References

- [Build run 42940218976](https://github.com/VMAFx/vmafx/actions/runs/42940218976) and [Builds run 42940218931](https://github.com/VMAFx/vmafx/actions/runs/42940218931): measured master macOS virtual-environment failures supplied with the fix brief.
- CPython source: `Mac/Tools/pythonw.c` (`__PYVENV_LAUNCHER__`, `_NSGetExecutablePath`) and `Modules/getpath.py` (`argv[0]` discovery); source-path evidence supplied with the brief, no network verification in this lane.
- Linux source: `include/linux/namei.h`, `MAXSYMLINKS = 40`; hop bound specified by the fix brief.
- Implementation and replayable cases: `scripts/lib/safe_subprocess.py`, `scripts/lib/test_safe_subprocess.py`, and `tools/rc1-tester/tests/test_safe_subprocess_copy.py`.
- Source: fix order `finding-repo-health-999f71af7d-20261111-0601`, 2026-11-11, trusted launch links (paraphrased).
