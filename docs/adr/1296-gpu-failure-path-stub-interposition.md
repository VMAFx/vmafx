<!-- markdownlint-disable MD013 -->

# ADR-1296: GPU init failure paths are tested device-free, by compiling the backend TU against runtime stubs

- **Status**: Accepted
- **Date**: 2026-09-22
- **Deciders**: Lusoris
- **Tags**: testing, hip, cuda, sycl, gpu, code-quality, agents

## Context

Every GPU backend extractor ends `init` with a ladder of failure branches that
release the stream, the events, the loaded modules and every device or pinned
allocation claimed so far. The ladder is the only thing standing between a
failed `init` and a use-after-free, because
`vmaf_feature_extractor_context_close` rejects an uninitialised context, so
`close` never runs after a failed `init` and nothing downstream reclaims a tier
the ladder skips.

Until now nothing tested those branches. The GPU tests in `core/test/` are
parity gates: they need a real device, run in the `gpu` suite, and skip on a
host without one. A failure branch cannot be reached from a parity gate at all
— the allocations it unwinds are exactly the ones that succeeded. T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22 is
what that gap costs: three branches across two HIP extractors released every
resource and then returned `0`, because the unwind ladder's terminal maps
`hipSuccess` to `0` and all three were handed `hipSuccess`. The framework set
`is_initialized` and the next `submit` ran on freed device memory. The defect
survived a HISS-01 refactor, an ADR-0759 change to the same path, and a written
`AGENTS.md` note describing it as intended behaviour.

## Decision

We will test GPU `init` failure paths by compiling the backend translation unit
directly into a test target that defines **every** symbol the TU references —
the runtime entry points, the embedded kernel blobs and the handful of libvmaf
helpers — and driving the failure from one of those stubs. The target links no
GPU runtime and needs no device, so it runs in the `fast` suite and never
skips. `core/test/test_hip_adm_init_unwind.c` and
`core/test/test_hip_ssimulacra2_init_unwind.c` are the reference
implementations.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Stub-interposed TU compiled into the test target (chosen) | Runs on any host in the `fast` suite; the stubs tally every allocation, so a skipped unwind tier fails as a leak, not just as a wrong return code; the stub surface is small and `nm -u` enumerates it exactly | A second definition of the extractor symbol, so the target must not link libvmaf; the stub list drifts if the TU gains a runtime call | Only option that actually fails when the bug is present, on a host with no AMD GPU |
| A `gpu`-suite test that forces a real allocation failure on the device | Exercises the genuine runtime | Needs hardware in CI; an OOM large enough to fail `hipMalloc` is not reliably reproducible; cannot reach the dictionary branch at all, which is a host allocation | Unreachable for the actual defect, and skips everywhere CI runs |
| `LD_PRELOAD` interposition of the HIP runtime | No source-level stubs | Linux-only, so the gate would skip on macOS and Windows (HISS-21); still needs the real ROCm runtime installed | Loses the platform neutrality the stub target gets for free |
| Command-line `-D` renames of the injected entry point | The pattern `test_framesync_init_failure` started from | A `-D` is in force before the headers are read, so it rewrites the *declaration* too — the failure mode that forced `test_framesync_interpose.h` | Defining the symbol in the test TU and linking no libvmaf removes the precondition instead of steering around it |
| Leave the paths untested | No new targets | Three live use-after-frees shipped undetected | Rejected on the evidence |

## Consequences

- **Positive**: a GPU `init` failure branch is now reachable from a test that
  runs everywhere. The ledger form of the assertion ("every pointer handed out
  came back") catches a skipped unwind tier, which a return-code assertion
  alone does not — the second defect in T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22 was exactly that, and the
  gate reproduces it independently.
- **Negative**: the test target compiles the extractor a second time and
  redefines its registration symbol, so it must never link `libvmaf`. The stub
  list has to track the TU's undefined symbols; when the TU gains a runtime
  call the target fails to link, which is a loud failure but still a
  maintenance tax. `nm -u <the TU's object>` regenerates the list.
- **Neutral / follow-ups**: the two HIP targets are gated on
  `enable_hipcc == true`, matching the `HAVE_HIPCC` guard the failure paths
  live behind, so the hip tidy lane (configured with `enable_hipcc=false`,
  see `T-TIDY-RATCHET-GPU-LANES-UNREPRODUCIBLE-2026-09-22`) does not measure
  them. The CUDA and SYCL ADM twins were audited for the same defect during
  T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22 and are clean, so no equivalent target was added for them; one
  should be if their unwind shape ever changes.

## References

- [`docs/state.md`](../state.md) row `T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22`; local ledger `BUG-092`.
- [ADR-0759](0759-hip-adm-buffer-by-pointer.md) — added `buf_dev` to the ADM unwind ladder.
- [ADR-1289](1289-hip-ssimulacra2-host-helper-split.md) — the SSIMULACRA2 host-helper split that introduced `ss2h_load_modules`.
- [ADR-1142](1142-whole-codebase-standards.md) — whole-tree standards; the new targets are lint-clean under the hip lane's clang-tidy profile.
- `core/test/test_framesync_interpose.h` — prior interposition target, and the `-D`-rename failure this ADR's approach avoids.
- `core/src/feature/hip/AGENTS.md` — the unwind-ladder invariants this gate protects.
