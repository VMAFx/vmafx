# Research 0128: DNN API execution-provider doc status

- **Date**: 2026-05-15
- **Workstream**: tiny-AI DNN API documentation hygiene
- **Tags**: dnn, tiny-ai, docs, execution-provider, fork-local

## Question

Does `docs/api/dnn.md` still describe the public `VmafDnnConfig`
execution-provider surface accurately?

## Findings

No. The page still described the early DNN runtime where `AUTO` was CPU-only,
OpenVINO / ROCm were accepted but ignored, and `fp16_io` was a ghost field.
Current `core/include/libvmaf/dnn.h` and `core/src/dnn/ort_backend.c`
show a broader runtime:

- `AUTO` tries CUDA, OpenVINO GPU, ROCm, CoreML, then CPU.
- OpenVINO supports the generic GPU-with-CPU-fallback selector plus pinned
  `NPU`, `CPU`, and `GPU` variants.
- CoreML supports the generic selector plus ANE, GPU, and CPU variants.
- ROCm has a generic EP append path.
- `fp16_io` stages FLOAT16 model slots and passes `precision=FP16` to
  OpenVINO.

The CLI-facing `docs/ai/inference.md` page already had the current matrix, so
the API page was the stale user-facing surface.

## Decision Matrix

| Option | Pros | Cons | Decision |
| --- | --- | --- | --- |
| Leave `docs/api/dnn.md` stale | No churn | Contradicts public header and CLI docs | Rejected |
| Remove the limitation bullets only | Removes the worst false statements | Still omits the append-only enum values | Rejected |
| Refresh the config block and limitations | Aligns the C API page with `dnn.h`, `ort_backend.c`, and `docs/ai/inference.md` | Documentation-only PR | Accepted |

## Reproducer

```bash
rg -n 'VMAF_DNN_DEVICE_|try_append_openvino|try_append_rocm|try_append_coreml|fp16_io' \
    core/include/libvmaf/dnn.h core/src/dnn/ort_backend.c docs/api/dnn.md
```
