# Research-0108: Vulkan VIF manual int64 subgroup reduction

## Question

Can the Vulkan VIF API-1.4 residual on NVIDIA RTX 4090 + driver
595.71.05 be closed by replacing `subgroupAdd(int64_t)` in
`vif.comp` with an explicit `subgroupShuffleXor` butterfly reduction?

## Context

[Research-0090](0090-vulkan-vif-fp-residual-phase-3b-2026-05-09.md)
left the `integer_vif_scale2` residual open after stronger shared-memory
fences failed. The observed failure was 45/48 mismatches at
`max_abs=1.527e-02` on NVIDIA device 0 under Vulkan API 1.4, with
run-to-run non-deterministic int64 accumulator magnitudes. Arc A380
and RADV were already clean.

The working hypothesis was that NVIDIA's int64 subgroup-add lowering,
not shared-memory ordering, was the remaining failure point.

## Change Tested

`core/src/feature/vulkan/shaders/vif.comp` now requires
`GL_KHR_shader_subgroup_shuffle` and routes the seven Phase-4 int64
subgroup reductions through:

```glsl
int64_t reduce_i64_subgroup(int64_t v) {
    int64_t r = v;
    for (uint mask = 1u; mask < gl_SubgroupSize; mask <<= 1u) {
        r += subgroupShuffleXor(r, mask);
    }
    return r;
}
```

The host descriptor layout, push constants, per-workgroup accumulator
layout, and CPU/CUDA/SYCL score contracts are unchanged.

## Results

Fresh local build:

```bash
meson setup build-vulkan-int64 libvmaf \
  -Denable_vulkan=enabled -Denable_cuda=false -Denable_sycl=false \
  -Denable_dnn=disabled -Denable_docs=false --buildtype=release
ninja -C build-vulkan-int64 tools/vmaf
```

Shader compile smoke:

```bash
glslc --target-env=vulkan1.3 -O \
  core/src/feature/vulkan/shaders/vif.comp -o /tmp/vif.spv
```

Cross-backend parity, NVIDIA RTX 4090 + driver 595.71.05:

```text
integer_vif_scale0  1.000000e-06  0/48 OK
integer_vif_scale1  1.000000e-06  0/48 OK
integer_vif_scale2  2.000000e-06  0/48 OK
integer_vif_scale3  2.000000e-06  0/48 OK
```

Repeated the NVIDIA gate five times; every run stayed 0/48 on every
scale with `integer_vif_scale2 max_abs=2.000000e-06`.

Regression lanes:

```text
Intel Arc A380 + Mesa-ANV 26.1.0:
integer_vif_scale0  1.000000e-06  0/48 OK
integer_vif_scale1  2.000000e-06  0/48 OK
integer_vif_scale2  2.000000e-06  0/48 OK
integer_vif_scale3  2.000000e-06  0/48 OK

RADV CPU + Mesa 26.1.0:
integer_vif_scale0  1.000000e-06  0/48 OK
integer_vif_scale1  2.000000e-06  0/48 OK
integer_vif_scale2  2.000000e-06  0/48 OK
integer_vif_scale3  2.000000e-06  0/48 OK
```

## Alternatives Considered

| Option | Pros | Cons | Decision |
| --- | --- | --- | --- |
| Keep `subgroupAdd(int64_t)` and wait for a driver fix | Zero shader churn | Leaves API-1.4 VIF blocked on NVIDIA | Rejected |
| Lower tolerance or skip NVIDIA | Would make CI pass | Violates the no-test-weakening rule and hides non-determinism | Rejected |
| Split int64 into int32 halves | Avoids int64 subgroup arithmetic | More code, carry handling, and rebase risk | Deferred; unnecessary after shuffle fix |
| Use `subgroupShuffleXor(int64_t)` butterfly | Minimal shader change, keeps int64 arithmetic exact, directly tests the Research-0090 hypothesis | Requires `GL_KHR_shader_subgroup_shuffle`, already available on the Vulkan target set | Chosen |

## Conclusion

The manual int64 subgroup butterfly closes the NVIDIA API-1.4 VIF
residual without regressing Arc or RADV. The result confirms the
Research-0090 hypothesis enough for this fork: the load-bearing
contract is now "do not restore `subgroupAdd(int64_t)` in
`vif.comp`'s Phase-4 accumulator path."

## References

- `req`: user asked to pick a big backlog/open item and continue real
  coding.
- [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md)
- [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md)
- [Research-0090](0090-vulkan-vif-fp-residual-phase-3b-2026-05-09.md)
