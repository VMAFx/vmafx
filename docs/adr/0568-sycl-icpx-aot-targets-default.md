# ADR-0568: Default `sycl_icpx_aot_targets` to full Intel arch list

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `sycl`, `build`, `meson`, `gpu`, `intel`, `aot`, `fork-local`

## Context

The SYCL backend is compiled with `icpx -fsycl`, which by default emits
portable SPIR-V device code. The Level Zero / IGC runtime JIT-compiles that
SPIR-V to native ISA on first use — a process that typically takes several
seconds and is paid again after driver upgrades or binary reinstallation.

For short VMAF runs (a handful of frames) this cold-start cost can dominate the
total wall time. For fleet deployments the cost is paid at every node startup
unless the IGC shader cache is pre-warmed.

The HIP backend learned the same lesson in PR #1329 (ADR-0561): the original
`hip_gfx_targets` default of `gfx90a` was broadened to cover common AMD GPU
generations and documented as a user-visible config knob. This ADR applies the
same pattern to the Intel SYCL path.

Per user direction, defaulting `sycl_icpx_aot_targets` to an empty string is
rejected as a silent performance footgun: any operator who builds with
`-Denable_sycl=true` without reading the documentation would silently receive
a binary that pays the JIT cold-start cost on every first launch. The default
must be default-active to protect operators who do not customise the option.

## Decision

Add a `sycl_icpx_aot_targets` Meson string option to `core/meson_options.txt`.
Its default value is a comma-separated list of Intel GPU micro-architecture
codenames covering:

- Arc discrete GPUs: `dg2-g10`, `dg2-g11`, `acm-g10`, `acm-g11`, `acm-g12`
- Tiger Lake through Battlemage iGPUs and mobile variants

When `sycl_compiler == 'icpx'` and the option is non-empty, the build passes
`-fsycl-targets=spir64_gen,spir64 -Xs '-device <list>'` to icpx. This embeds
native ISA blobs for every listed target plus a SPIR-V fallback for unlisted
devices. When the option is empty, only `-fsycl` is passed (JIT only).

The option is ignored for AdaptiveCpp (`acpp`) builds.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Default empty (JIT only) | Smallest binary; works on any Intel GPU | Silent cold-start penalty on every first launch; catches operators by surprise (same trap as HIP pre-PR#1329) | Rejected per user direction as a silent perf footgun |
| Single target (`dg2-g11`) | Smaller binary than full list | Misses iGPU targets; wrong default for desktop / laptop iGPU operators | Too narrow for a general default |
| Auto-detect at configure time (like HIP `rocm_agent_enumerator`) | Detects the build host GPU | Configure-time GPU may differ from deploy target; cross-compiling fails | The Intel SYCL toolchain does not expose a stable query API comparable to `rocm_agent_enumerator`; ship a broad list instead |
| AdaptiveCpp AOT in same PR | Symmetric with icpx fix | AdaptiveCpp `intel_gpu_<arch>` target support requires acpp 23.10+; version matrix unclear | Deferred; tracked as a Known gap in `docs/backends/sycl/overview.md` |

## Consequences

- **Positive**: operators who build with `-Denable_sycl=true` get AOT native
  ISA out of the box; first-launch cold-start eliminated for all listed targets.
- **Positive**: the fat binary embeds a SPIR-V fallback, so future or unlisted
  devices still work without rebuilding.
- **Positive**: single-target fleets can override with
  `-Dsycl_icpx_aot_targets='dg2-g11'` for a smaller binary.
- **Negative**: build time increases (icpx must invoke IGC for each listed
  target); expect several minutes of additional compile time versus JIT-only.
- **Negative**: binary size increases by approximately 10–30 MB depending on
  how many targets are included.
- **Neutral**: `lnl-m` and `bmg-*` require icpx 2025.0+ / 2025.1+
  respectively; older toolchains silently skip those targets with an
  "unknown device" warning.

## References

- PR #1329 / ADR-0561: HIP `gfx_targets` broadening (direct analogue).
- Per user direction: defaulting to empty means any operator building without
  explicitly setting the flag silently ships a SPIR-V-JIT-only binary that pays
  cold-start cost on every first kernel launch — paraphrased to professional
  English per CLAUDE.md user-quote rule. The deciding factor is rejecting the
  default-empty option as a silent performance footgun.
- Intel oneAPI DPC++ Compiler SYCL AOT documentation:
  <https://www.intel.com/content/www/us/en/docs/dpc-cpp-compiler/developer-guide-reference/current/ahead-of-time-compilation.html>
- Intel GPU codename reference (IGC `-device` values):
  <https://github.com/intel/intel-graphics-compiler/blob/master/documentation/visa/instructions/PLATFORM.md>
