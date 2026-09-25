- Bound `vmaf-perShot` scan loop with operator frame ceiling (`-F, --frames`),
  defaulting to 0 (unbounded compatibility contract). Bounded reads on FIFOs,
  streams, and `/dev/zero` now terminate cleanly after N frames with exit code 0
  without hanging. Also resolves the `UINT32_MAX` off-by-one boundary check from
  ADR-1287, safely accepting exactly `UINT32_MAX` frames (ADR-1318).
