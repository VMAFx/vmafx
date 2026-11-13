<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/sycl

Orientation for agents working on SYCL / DPC++ backend runtime. Parent rules apply in full: [../../AGENTS.md](../../../AGENTS.md).

- Compiler = `icpx` (oneAPI DPC++) or AdaptiveCpp `acpp` / `syclcc` ([ADR-0407](../../../../docs/adr/0407-adaptivecpp-second-sycl-toolchain.md)); `clang++ -fsycl` not CI-tested; do not assume MSVC-style extensions.
- Experimental SYCL flags enabled: `-fsycl-unnamed-lambda`, `-fsycl-allow-func-ptr`, `-fsycl-device-code-split=per_kernel` ([ADR-0027](../../../../docs/adr/0027-non-conservative-image-pins.md)).
- USM allocations: prefer device USM for kernel-resident data, shared USM for cross-side communication, host USM only when adjacent API requires host-dereferenceable pointer.
- Numerical snapshots: kernels unable to bit-match CPU reference regenerate `testdata/scores_cpu_sycl.json` via [`/regen-snapshots`](../../../../.claude/skills/regen-snapshots/SKILL.md) with commit justification ([AGENTS.md §5](../../../../AGENTS.md)).
