<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1209: `--gpumask` keeps rejecting negative values; the test script uses a positive mask

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cli, testing, upstream-divergence, correctness

## Context

`core/tools/test/test_vmaf_cuda_gpumask.sh` is inherited verbatim from upstream
and invokes `--gpumask -1` twice, commented "gpumask: use cpu". The fork's CLI
rejects it:

```text
Invalid argument "-1" for option --gpumask; should be a non-negative integer
```

The script runs under `set -e`, so `test_vmaf_cuda_gpumask` fails on **any host
that actually has an NVIDIA GPU**. It reports green in CI only because the
script exits 77 (meson SKIP) when `nvidia-smi -L` finds no device.

`-1` only ever "worked" upstream by accident. Upstream's `parse_unsigned` calls
`strtoul` directly, and POSIX `strtoul` silently converts `"-1"` to `ULONG_MAX`
without setting `errno`; that then truncates to `UINT_MAX`. The fork
deliberately closed that hole (`core/tools/cli_parse.cpp::parse_unsigned`
rejects a leading `'-'` before calling `strtoul`, with a comment saying exactly
why). So the CLI is behaving as designed and the script is the stale side.

The semantics make a positive mask the correct spelling anyway. `gpumask` is
documented in `libvmaf.h` as: *any non-zero value disables the GPU
feature-extractor selection for both the CUDA and SYCL backends (the runtime
falls back to the CPU implementation)*. It is not a per-op bitmask despite the
`<bitmask>` placeholder in the usage string.

Measured on an RTX 4090 over the Netflix 576x324 pair, 4 frames:

| invocation | pooled VMAF |
|---|---|
| `--gpumask 0` | 88.80022305138327 |
| `--gpumask 1` | 88.8002154453433 |
| `--no_cuda --no_sycl` | 88.8002154453433 |

`--gpumask 1` is byte-identical to an explicit CPU run, which is exactly what
the script's `-1` was reaching for.

## Decision

We will keep the CLI's rejection of negative `--gpumask` values and change the
test script to use `--gpumask 1`, the documented "any non-zero" spelling. The
`--gpumask` entry in `docs/usage/cli.md` is corrected at the same time: it
described a per-op mask, which the option has never been.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep strict parsing, fix the script to `1` (chosen) | The CLI keeps failing loudly on input the caller did not mean; the script states its intent directly; matches the documented API semantics | Diverges from upstream's accidental acceptance of `-1` | — |
| Special-case `--gpumask` to accept negatives as "all bits set" | Restores upstream-compatible spelling | Reintroduces exactly the silent unsigned wraparound the fork removed on purpose, for one option; CERT INT and the fork's own coding standards forbid the implicit conversion | Rejected |
| Relax `parse_unsigned` globally | One change covers any future case | Would silently accept `-1` for `--width`, `--threads`, `--frame_cnt` and every other unsigned option — a much worse footgun | Rejected |
| Delete the script's `-1` invocations | Trivially green | Loses coverage of the CPU-fallback path, which is the thing the script exists to test | Rejected — never remove a user surface's coverage to make a gate pass |

## Consequences

- **Positive**: `test_vmaf_cuda_gpumask` passes on a GPU host instead of only
  skipping on a GPU-less one. Verified: `rc=0` on the RTX 4090 workstation,
  where it failed before.
- **Negative**: anyone who scripted `--gpumask -1` against upstream gets a hard
  error on this fork. That is pre-existing — the fork has rejected it since
  `parse_unsigned` was hardened — and the error message names the constraint.
  `docs/usage/cli.md` now documents the divergence.
- **Neutral / follow-ups**: the usage string still calls the argument
  `$bitmask`. It is left alone here because changing the help text is a
  user-visible string change with its own compatibility surface; the reference
  table in `docs/usage/cli.md` carries the accurate description.

## References

- `core/tools/cli_parse.cpp::parse_unsigned` — the deliberate negative
  rejection and its rationale comment.
- `core/include/libvmaf/libvmaf.h` — the `gpumask` "any non-zero" contract.
- Upstream ships the same script and the same `strtoul`-based parser:
  `libvmaf/tools/test/test_vmaf_cuda_gpumask.sh`,
  `libvmaf/tools/cli_parse.c`.
- Source: `req` — user direction to fix the `--gpumask -1` regression found
  while running the full suite for ADR-1204 / ADR-1205.
