# Research 0120: Runtime doc-status audit batch

## Question

Which user-facing docs still carry stale scaffold / placeholder / TBD
wording after the recent MCP, Metal, vmaf-tune, and tiny-AI model
promotions?

## Findings

### Embedded MCP

- `docs/mcp/index.md` and `docs/mcp/embedded.md` already describe the
  current runtime: stdio, UDS, loopback SSE, `list_features`, and
  `compute_vmaf` are live; mutating measurement-thread tools and the
  SPSC bridge remain future work.
- `docs/api/mcp.md` still described the old ADR-0209 scaffold state:
  every entry point returning `-ENOSYS`, runtime bodies pending
  T5-2b, and a fallback-only recommendation.
- `docs/development/build-flags.md` still labelled `enable_mcp` and all
  transport sub-flags as stub-only. It also listed `enable_mcp_sse` as a
  boolean defaulting false, while `core/meson_options.txt` now defines
  it as a feature option defaulting `auto`.
- `libvmaf/AGENTS.md` still carried the pre-runtime rebase invariant
  saying `src/mcp/mcp.c` was a stub TU whose entry points validate and
  then return `-ENOSYS`.

### Metal

- `docs/backends/metal/index.md` already described the current runtime
  and first eight feature kernels.
- `docs/backends/index.md`, `docs/api/index.md`, `docs/api/gpu.md`,
  and the `enable_metal` Meson option description still described
  Metal as a scaffold whose public entry points return `-ENOSYS`
  until T8-1b.
- `docs/backends/vulkan/moltenvk.md` still said native Metal would be
  available only once the scaffold landed.

### Tiny-AI and vmaf-tune

- `docs/ai/benchmarks.md` still published a "Placeholder scoreboard"
  with `TBD` rows even though the registry and model cards now carry
  real shipped-score summaries for `fr_regressor_v*` and
  `vmaf_tiny_v2/v3/v4`.
- `docs/usage/vmaf-tune-bisect.md` still said current subcommands were
  "stubbing out" the Phase-B bisect via a placeholder predicate. The
  implementation is now production wiring; only custom predicates stay
  as test/extension hooks.

## Decision

Refresh the stale public docs, build-option descriptions, and invariant
notes without changing runtime code:

- API docs now state which entry points and tools are live, and limit
  `-ENOSYS` to builds that omit `-Denable_mcp=true`.
- Build-flag docs now match the current Meson option types and
  transport behaviour.
- `core/meson_options.txt` descriptions stop promising future bodies
  that already landed.
- `libvmaf/AGENTS.md` now preserves the actual runtime invariants:
  early argument validation, `compute_vmaf` using an ephemeral
  `VmafContext`, and the default-off umbrella flag until mutating tools
  and the SPSC bridge land.
- Metal docs now describe the live Apple-Silicon runtime and first
  kernel batch, while keeping remaining metric kernels as follow-up
  work.
- Tiny-AI benchmarks now publish a registry-backed shipped-score
  snapshot instead of a global TBD runtime table.
- vmaf-tune bisect docs now describe the placeholder predicate as
  historical, not current.

## Reproducer

```bash
rg -n 'every entry point currently returns|Stub-only until T5-2b|all `libvmaf_metal.h` entry points return|all entry points return `-ENOSYS` until|4 consumers registered|Placeholder scoreboard|stubbing out via the placeholder predicate|Large / throughput-bound runs land on Metal once the runtime PR ships|Build — macOS Metal \(T8-1 scaffold\)' \
  docs/api docs/backends docs/development docs/usage docs/ai \
  core/meson_options.txt libvmaf/AGENTS.md
```

## References

- `req`: user asked to keep finding backlog/open/scaffold/stub/doc gaps
  now that the previous train had merged.
- [ADR-0209](../adr/0209-mcp-embedded-scaffold.md)
- [ADR-0332](../adr/0402-mcp-runtime-v2.md)
