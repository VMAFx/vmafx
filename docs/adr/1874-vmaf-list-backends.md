<!-- markdownlint-disable MD013 MD060 -->
# ADR-1874: The vmaf CLI reports its usable backends; the score-backend selectors read that report

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: maintainer
- **Tags**: cli, vmaf-tune, go, gpu, cuda, sycl, hip, metal, fork-local

## Context

Two selectors pick the libvmaf backend for a tuning run:
`tools/vmaf-tune/src/vmaftune/score_backend.py` and its Go port
`pkg/scorebackend` (used by `vmafx-tune`). Each decided "usable" the same way
([ADR-0667](0667-vmaf-tune-score-backend-native-priority.md)): a backend had to
appear in the `--backend` line of `vmaf --help`, and a vendor tool
(`nvidia-smi`, `sycl-ls`, `rocminfo` / `rocm-smi`) had to see a device.

The help text names every backend whatever the build, so the first condition
never excluded anything; the vendor tools answer for the host, not for the
binary. On a host with an NVIDIA driver and ROCm, a CPU-only `vmaf` was
offered `cuda` and `hip`, `auto` chose `cuda`, and `vmaf --backend cuda` then
refused the run (ADR-0498). Neither selector knew `metal`, which the CLI has
accepted since the Metal backend landed. The two implementations had to be
kept equal by hand. The RC4 API review asked for one implementation (HISS-19),
with the other a thin call or a generated table.

## Decision

The `vmaf` CLI gains `--list-backends`: it prints one JSON document listing
cpu, cuda, sycl, hip and metal, in that order, each with `compiled` (built
into this binary) and `usable` (the backend's state initialiser succeeds on
its default device, the call a scoring run makes; cpu always), plus
`init_status` for a compiled backend that did not initialise. The option needs
no inputs and exits like `--version`.

Both selectors read that report and nothing else: `detect_available_backends()`
and `scorebackend.Detect()` run `vmaf --list-backends`, and the vendor-tool
probes and help-text parsing are removed (the exported Go
`ParseSupportedBackends` stays, deprecated). The `auto` chain becomes
`cuda, sycl, hip, metal, cpu`; an explicit choice stays strict. When the
report is unavailable (a `vmaf` older than this option), only cpu is usable
and a warning names the reason. The selection policy that remains in each
language is a few lines; both replay one case table,
`testdata/score_backend_selection.json`.

## Alternatives considered

| Option | Pros | Cons | Outcome |
|---|---|---|---|
| The CLI reports, both selectors read the report (**chosen**) | One authority, which is the binary that will score; uses the state initialisers a run uses; covers every backend the CLI knows, metal included; no vendor tools needed | A new CLI option; a GPU state is initialised once per report | Chosen |
| Keep the Python module, make the Go package call it | No CLI change | `vmafx-tune` would need a Python interpreter; still guesses from the help text and vendor tools | Not chosen |
| Generate both probe tables from one data file | Probe commands and tokens defined once | Still asks vendor tools about the host, not the binary; the CPU-only-build defect stays | Not chosen |
| Run a short scoring job per backend (as the MCP `probe_backend` tool does) | Proves the full path | Seconds per backend and a model load each; a third implementation | Not chosen |

## Consequences

- **Positive**: `auto` never offers a backend the binary cannot run; Metal is
  selectable; the selectors cannot drift in what "usable" means.
- **Negative**: `--list-backends` initialises each compiled GPU backend once
  (up to about a second each); callers run it once per tuning command.
- **Neutral / follow-ups**: the MCP servers still read the help text for
  `list_backends` (it names every backend on every build); moving them to the
  report belongs to the RC5 tool consolidation. RC4 WP3 exposes device
  enumeration in the new API; the CLI report can then be built on it.

## References

- Maintainer direction (2026-10-05, paraphrased): fix `pkg/scorebackend`
  duplicating `score_backend.py` as a bug fix into the train, keeping one
  implementation and the other as a thin call or a generated table.
- [ADR-0667](0667-vmaf-tune-score-backend-native-priority.md) (superseded in
  part: the availability probe and the `auto` chain),
  [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md),
  [ADR-0726](0726-drop-vulkan-backend.md); ADR-1852 (#2176)
