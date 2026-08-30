<!-- markdownlint-disable MD060 -->
# ADR-0690: VMAFX Binary and AI Tool Aliases

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vmafx, rebrand, cli, build, ai, mcp

## Context

ADR-0686 (VMAFX rebrand umbrella) locked the decision to introduce a `vmafx`
binary that applies modernized defaults: `--precision=max` (IEEE-754 lossless
output, `%.17g`), auto-backend detection (CUDA > Vulkan > SYCL > CPU — already
the CLI default since ADR-0498), and a distinct banner that names the active
defaults on startup.

Two approaches to the `vmafx` binary exist: (A) a symlink to the existing
`vmaf` binary that detects its invocation name via `argv[0]` basename
inspection, or (B) a separate `vmafx.c` that `exec()`s `vmaf` with prepended
arguments. Option A produces one binary on disk, zero code duplication, and
no extra fork/exec overhead. The `argv[0]` detection pattern is well-
established on POSIX systems (e.g., busybox, ffmpeg multi-call).

For the Python tools (`vmaf-train`, `vmaf-tune`, `vmaf-mcp`), the cleanest
alias mechanism is a second `[project.scripts]` entry pointing to the same
callable. pip/hatch installs both entry-point scripts at package install time;
the scripts are byte-identical thin wrappers; no import-side detection is
required because the Python tools have no session-startup banner that needs
to vary.

## Decision

We apply Option A for the C binary: Meson's `install_symlink()` installs
`vmafx` → `vmaf` in `bindir`; `cli_parse.c` adds `detect_vmafx_mode(argv[0])`
which inspects the basename, sets `settings->vmafx_mode = true`, and after the
getopt loop applies `precision_max = true` / `precision_fmt = "%.17g"` unless
the user passed an explicit `--precision` flag. The startup banner prints
`"VMAFX version <V> (precision=max)"` in vmafx mode.

For the AI tools we add `vmafx-train`, `vmafx-tune`, and `vmafx-mcp` as
additional `[project.scripts]` entries (pointing to the same callables as
the existing `vmaf-*` entries) in `ai/pyproject.toml`,
`tools/vmaf-tune/pyproject.toml`, and `mcp-server/vmaf-mcp/pyproject.toml`.

The existing `vmaf`, `vmaf-train`, `vmaf-tune`, and `vmaf-mcp` entry points
are **not changed**: this is purely additive.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Option A — symlink + argv[0] | One binary; no exec overhead; clean | Slightly non-obvious for maintainers unfamiliar with multi-call patterns | **Chosen** — minimal on-disk footprint, established pattern |
| Option B — separate vmafx.c exec()s vmaf | Clean separation; no argv[0] logic | Extra fork/exec per invocation; two binaries to maintain; Windows exec() semantics differ | Rejected — overhead not justified when argv[0] detection suffices |
| Option C — vmafx.c with duplicated main() | No runtime detection needed | Full code duplication; two diverging binaries over time | Rejected — maintenance burden unacceptable |
| Python: wrapper scripts in bin/ | Simple shell scripts | Platform-dependent (no native Windows); not pip-installable | Rejected — console_scripts is the idiomatic pip mechanism |

## Consequences

- **Positive**: `vmafx --version` shows version + identity; `vmafx` applies
  precision=max by default without extra flags; both `vmaf-*` and `vmafx-*`
  names work after pip install.
- **Negative**: Maintainers must keep the `vmafx_mode` branch in `cli_parse.c`
  updated when new defaults are added to vmafx mode. The `--version` output
  differs between `vmaf -v` and `vmafx -v` — CI scripts that parse version
  strings must accept both prefixes.
- **Neutral**: The `install_symlink()` call requires Meson >= 0.61 (already
  satisfied; the project pins Meson >= 1.0). Downstream packagers who split
  `vmaf` and `vmafx` into separate packages must install both to the same
  `bindir` to keep the symlink valid.

## References

- ADR-0686: VMAFX rebrand umbrella (parent decision).
- ADR-0119: `--precision` flag and `%.17g` lossless format.
- ADR-0498: `--backend` exclusive selector and auto-backend default.
- req: "Option A (preferred): meson installs vmafx as a symlink to vmaf, then
  vmaf argv[0]-detects 'vmafx' and applies modernized defaults inline."
