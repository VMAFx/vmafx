<!-- markdownlint-disable MD013 MD046 MD060 -->
# Recipe — Score on two GPUs

**Situation:** the machine has more than one GPU and you want to pin which
one scores — or run two scorings in parallel, one per device.

## Picking a device

Backend auto-selection takes the first registered device; pin a different
one explicitly:

```bash
# CUDA: device index follows CUDA_VISIBLE_DEVICES
vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 \
    -m version=vmaf_v0.6.1 --backend cuda --json -o cuda0.json

# SYCL: device index is explicit on the flag
vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 \
    -m version=vmaf_v0.6.1 --backend sycl --sycl_device 1 --json -o sycl1.json
```

For multi-GPU CUDA, drive the device selection from the environment instead
of the flag, so the same command runs on either card:

```bash
CUDA_VISIBLE_DEVICES=0 vmaf ... --backend cuda -o dev0.json &
CUDA_VISIBLE_DEVICES=1 vmaf ... --backend cuda -o dev1.json &
wait
```

## Reading it

- The two runs are **bit-exact** against each other and the CPU when the
  device twin is marked exact — check the twin status for your features in
  the [feature index](../metrics/features.md) before comparing numbers
  across devices.
- GPU scores are sensitive to a busy desktop stack: quiet-machine numbers
  first, then compare ([FAQ: GPU acceleration](../reference/faq.md#q-does-the-fork-support-gpu-acceleration)).
- `--list-backends` prints what this build registered; a backend absent at
  build time (no CUDA/SYCL compiled in) cannot be selected at runtime.

This recipe carries no `run-check` blocks: it needs real GPU hardware, so
the docs check has nothing to execute in a CPU-only environment — the
commands above are pinned by the same flag-surface review as the rest of the
docs.

See the [glossary](../reference/glossary.md) — terms used here:
[default model](../reference/glossary.md#default-model).
