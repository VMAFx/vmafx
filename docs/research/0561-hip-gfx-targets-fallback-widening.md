<!-- markdownlint-disable MD060 -->
# Research digest: ADR-0561 — HIP gfx_targets fallback widening

## Problem characterisation

`core/src/meson.build` discovers the AMD GPU ISA targets for `hipcc --genco`
through a four-step probe chain. When all dynamic probes fail (typical in
no-GPU build sandboxes), the build fell back to a hardcoded string. That string
was `gfx90a` — a single CDNA2 server target — which worked for AWS and GCP GPU
instances but not for the consumer AMD GPUs on the fork's primary development
host.

The dev host has an AMD Raphael APU with an integrated GPU presenting as
`gfx1036`. ROCm's allowlist gate requires `HSA_OVERRIDE_GFX_VERSION=10.3.0`,
which maps the iGPU to `gfx1030` for device-code dispatch. At runtime the
loader looked for an HSACO blob compiled for `gfx1030`; the `gfx90a`-only fat
binary contained no compatible object and emitted:

```text
hip_fatbin.cpp: No compatible code objects found for: gfx1030
```

The error surfaced after every clean container rebuild (BuildKit has no GPU
bind-mount, so `rocm_agent_enumerator` and `hipconfig` both returned empty). The
`libvmaf.so` compiled fine, but the HIP backend silently failed to load the
feature kernels at runtime.

## Target landscape

The widened fallback targets `gfx90a,gfx1030,gfx1036,gfx1100`:

| Target | Architecture | Typical device |
|--------|-------------|----------------|
| `gfx90a` | CDNA2 | AMD Instinct MI200 (data-centre GPU) |
| `gfx1030` | RDNA2 | RX 6000 desktop + Raphael APU override |
| `gfx1036` | RDNA2 (iGPU) | AMD Raphael APU (RX 680M / Radeon 680M) |
| `gfx1100` | RDNA3 | RX 7000 desktop |

`gfx1036` is the native ISA of the Raphael iGPU. Including it in the fat binary
means the device-code load succeeds even without `HSA_OVERRIDE_GFX_VERSION`
(though the env var is still required for `hsa_init()` on ROCm 6.x).

## Fat binary size impact

A four-target fat binary is approximately 3× larger than a single-target binary:

| Kernel | gfx90a only | gfx90a + gfx1030 + gfx1036 + gfx1100 |
|--------|-------------|---------------------------------------|
| `vif_statistics` | ~180 KB | ~620 KB |
| All kernels combined | ~1.4 MB | ~4.8 MB |

Total `libvmaf.so` delta: < 4 MB. Accepted as negligible for a development
build; production operators who need smaller binaries can pin via
`-Dhip_gfx_targets=gfx1036`.

## Verification

Build in a no-GPU sandbox and confirm the fat binary contains all four target
objects:

```bash
hipcc --genco --offload-arch=gfx90a --offload-arch=gfx1030 \
      --offload-arch=gfx1036 --offload-arch=gfx1100 \
      -I libvmaf/src -I core/src/feature -I libvmaf/include \
      core/src/feature/hip/integer_vif/vif_statistics.hip \
      -o /tmp/vif_stats.hsaco
# Inspect targets:
python3 -c "
import struct, sys
with open('/tmp/vif_stats.hsaco','rb') as f: data=f.read()
print([hex(o) for o in range(0,len(data),4) if b'gfx' in data[o:o+8]])
"
```

Runtime smoke test (requires AMD GPU on host):

```bash
docker exec vmaf-dev-mcp /workspace/build-hip/core/tools/vmaf \
  --backend hip \
  --reference /workspace/testdata/ref_576x324_48f.yuv \
  --distorted /workspace/testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --json /tmp/hip_gfx.json
# Must not emit "No compatible code objects found"
```
