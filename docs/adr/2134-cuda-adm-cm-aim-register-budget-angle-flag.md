<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2134: `adm_cm_aim_line_kernel_4` gets its own register budget of 209 for the exact scale-0 angle flag

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: lusoris
- **Tags**: cuda, hip, adm, exactness, build

## Context

[ADR-1226](1226-cuda-adm-cm-aim-grid-occupancy.md) fixes a blanket budget of 208 registers per thread for every kernel of `adm_cm.fatbin`, with zero stack and local spill, and `test_cuda_adm_cm_register_pressure` enforces it. The scale-0 angle flag of the CUDA and HIP ADM twins (`decouple_angle_flag_s0()` in `adm_decouple_inline.cuh` / `.hip`) added int16 products in int32: with every band at -32768 the sums are 2^31 and wrap, where the CPU's `adm_angle_flag()` sums in int64. 17 of the 256 corner combinations of the int16 range gave another flag than the CPU on both twins (`T-GPU-ADM-ANGLE-FLAG-S0-INT32-CORNER-2026-10-06`). The flag selects the gain-limited branch of the decouple, so the twins were not bit-identical to the CPU there.

Four exact forms were measured in `adm_cm_aim_line_kernel_4`, the one kernel at the budget on master (208): plain int64 sums 216 registers; an unsigned sum with `- 1` then `+ 1` in 64 bits 209; a select on INT32_MIN 210; unsigned magnitudes with a signed dot product 210. A fifth form raised `adm_cm_line_kernel_8` to 229 and was dropped.

## Decision

Use the unsigned-sum form (209) in both twins: the dot product is an unsigned sum of the two int32 products, `(int64_t)(int32_t)(sum - 1u) + 1` restores the one value an int32 cannot hold, and the squared magnitudes are unsigned sums widened to int64. The flag equals the CPU's at every corner. Exactness is the bar (AGENTS.md section 11: speed lost to exactness in RC3 is a tuning row recovered in RC7, never traded for tolerance).

The budget becomes per kernel: `test_cuda_adm_cm_register_pressure` keeps 208 for every kernel and sets `adm_cm_aim_line_kernel_4` to its measured 209 (the maximum over the architectures of the fatbin; 208 before). Spill is zero: `cuobjdump -res-usage` reports STACK 0 and LOCAL 0 for every kernel and architecture, checked on the fatbin before and after. No other kernel changes budget (`adm_cm_line_kernel_8` is 149 at most). A tuning row, `T-CUDA-ADM-CM-AIM-KERNEL-4-REGISTERS-RC7-2026-10-06`, wins the register back in RC7.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Plain int64 sums | Simplest | 216 registers, 8 over the budget | Costs more occupancy than the cheaper exact form |
| Select on INT32_MIN | Readable | 210 registers | One more than the chosen form |
| Keep int32, record the corners | No budget change | The twin differs from the CPU at 17 corners | Exactness is the bar |
| Blanket budget of 216 | One number | Hides regressions of the other kernels | A budget per kernel keeps the others at 208 |
| Unsigned sum, `- 1` / `+ 1` (chosen) | Exact, 209 registers | Needs the comment explaining the trick | |

## Consequences

- **Positive**: the CUDA and HIP scale-0 angle flag equals the CPU's at every int16 corner; `test_adm_decouple_recip_{cuda,hip}` expects 0 corner mismatches (17 before).
- **Negative**: `adm_cm_aim_line_kernel_4` uses one more register on one architecture, which can lower its occupancy there.
- **Neutral / follow-ups**: RC7 tuning row to recover the register; a later change to the header re-measures all kernels with `test_cuda_adm_cm_register_pressure`.

## References

- req (paraphrased): fix the CUDA and HIP angle-flag corner now, with the cheapest exact form, a per-kernel budget amending ADR-1226, a tuning row for RC7 and device evidence (coordinator decision, 2026-10-06).
- [ADR-1226](1226-cuda-adm-cm-aim-grid-occupancy.md): the blanket budget this amends.
