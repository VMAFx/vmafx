<!-- markdownlint-disable MD060 -->
# `vmafx` — modernized CLI reference

`vmafx` is a thin alias for the `vmaf` binary that activates modernized
defaults on invocation. It is installed as a symlink to `vmaf` in the same
`bindir` (or a dedicated binary on Windows); the binary detects the `vmafx`
basename at startup and adjusts its behavior accordingly (ADR-0690).

> **Relationship to `vmaf`.** `vmafx` and `vmaf` will share one binary on disk.
> Every flag documented in [`cli.md`](cli.md) is also accepted by `vmafx`.
> The only differences are the defaults described on this page. To restore all
> legacy defaults with `vmafx`, pass `--netflix-compat` explicitly.

## Modernized defaults

| Behavior | `vmaf` default | `vmafx` default |
|---|---|---|
| Output precision | `%.6f` (6 decimal places, matches upstream Netflix) | `%.17g` (IEEE-754 round-trip lossless, `--precision=max`) |
| Backend selection | auto (SYCL > CUDA > HIP > CPU) | same — auto is the default for both |
| Startup banner | `VMAF version <V>` | `VMAFX version <V> (precision=max)` |
| `--version` output | `<version string>` | `VMAFX <version string> (auto-backend, precision=max)` |

The backend auto-selection priority (SYCL > CUDA > HIP > CPU) applies to
both `vmaf` and `vmafx` and reflects the libvmaf registry order established
after ADR-0726 dropped Vulkan. No difference exists at the backend level
between the two binaries.

## Why `--precision=max` is the vmafx default

The Netflix-upstream `%.6f` precision preserves bit-for-bit compatibility with
the three golden-data test pairs in `python/test/`. This is essential for the
`vmaf` binary, which participates in the golden-data gate.

`vmafx` targets new workflows that consume VMAF scores programmatically and
benefit from the full IEEE-754 double range. `%.17g` outputs the minimum
number of significant digits required for exact round-trip through `strtod`,
avoiding silent truncation when scores are stored in JSON, CSV, or database
tables. See [precision.md](precision.md) for full details (ADR-0119).

## Quick start

```shell
# .y4m pair — precision=max applied automatically
vmafx --reference ref.y4m --distorted dist.y4m

# Equivalent vmaf invocation with explicit flag
vmaf --reference ref.y4m --distorted dist.y4m --precision=max

# Opt back into legacy %.6f precision via explicit flag
vmafx --reference ref.y4m --distorted dist.y4m --precision=legacy

# Check which version and defaults are active
vmafx --version
# Output: VMAFX v3.2.1 (auto-backend, precision=max)
```

## `--netflix-compat` — restore legacy defaults

`--netflix-compat` is a single flag that restores the complete set of
Netflix-upstream legacy defaults, regardless of whether the binary was invoked
as `vmaf` or `vmafx`. It is the recommended way to ensure output that is
bit-for-bit identical to upstream Netflix/vmaf.

When `--netflix-compat` is passed, the following defaults are forced as the
**final** post-parse pass (overriding any vmafx-mode modernizations):

| Setting | `--netflix-compat` forced value |
|---|---|
| Backend | CPU only (equivalent to `--backend=cpu`) |
| Output precision | `%.6f` (equivalent to `--precision=legacy`) |
| Default model | `vmaf_v0.6.1` (restores legacy upstream default model) |

The flag is idempotent on the `vmaf` binary — `vmaf` already uses these
defaults, so `vmaf --netflix-compat` is a no-op in practice.

```shell
# Restore legacy defaults on vmafx (overrides precision=max and auto-backend)
vmafx --reference ref.y4m --distorted dist.y4m --netflix-compat

# Equivalent explicit form (both flags together)
vmafx --reference ref.y4m --distorted dist.y4m --backend=cpu --precision=legacy

# On the vmaf binary — idempotent, same output as without the flag
vmaf --reference ref.y4m --distorted dist.y4m --netflix-compat
```

Both `--netflix-compat` and `--precision=legacy` produce `%.6f`-formatted
scores. The difference is that `--netflix-compat` also forces `--backend=cpu`,
ensuring the CPU golden-data path is taken regardless of available GPU hardware.

### When to use `--netflix-compat`

Use `--netflix-compat` when:

- Reproducing results for comparison with upstream Netflix/vmaf.
- Running a workflow that must satisfy the three golden-data test assertions
  in `python/test/` (CPU + `%.6f` precision is required — see CLAUDE.md §8).
- Debugging a numeric difference between `vmafx` and `vmaf` output: adding
  `--netflix-compat` to the `vmafx` invocation isolates whether the difference
  is from backend (GPU vs CPU) or precision (`%.17g` vs `%.6f`).

## AI tool aliases

Three companion Python tools also ship `vmafx-*` aliases alongside their
existing `vmaf-*` names. Both names invoke the same callable:

| `vmaf-*` name | `vmafx-*` alias | Package |
|---|---|---|
| `vmaf-train` | `vmafx-train` | `ai/` (hatch package `vmaf-train`) |
| `vmaf-tune` | `vmafx-tune` | `tools/vmaf-tune/` (hatch package `vmaf-tune`) |
| `vmaf-mcp` | `vmafx-mcp` | `mcp-server/vmaf-mcp/` (hatch package `vmaf-mcp`) |

Install any package with `pip install -e <path>` to get both names. The
aliases are console_scripts entries pointing to the same Python callables;
no behavior difference exists between `vmaf-train` and `vmafx-train`.

## Smoke test

After installation, confirm the symlink and default banner:

```shell
# Confirm vmafx resolves to the vmaf binary
ls -la $(which vmafx)
# -> ... vmafx -> vmaf

# Version string shows VMAFX identity and defaults
vmafx --version
# -> VMAFX v3.2.1 (auto-backend, precision=max)

# Or inside the dev container:
docker exec vmaf-dev-mcp vmafx --version
```

## See also

- [`cli.md`](cli.md) — full flag reference for all options accepted by both
  `vmaf` and `vmafx`.
- [`precision.md`](precision.md) — detailed explanation of `--precision` modes
  (ADR-0119).
- [ADR-0690](../adr/0690-vmafx-binary-and-ai-aliases.md) — decision record for
  the symlink implementation and argv[0] detection mechanism.
- [ADR-0696](../adr/0696-vmafx-netflix-compat.md) — decision record for the
  `--netflix-compat` flag design and post-parse ordering.
- [ADR-0686](../adr/0686-vmafx-rebrand-aggressive-modernization.md) — VMAFX
  rebrand umbrella ADR.
