# Research 0121: MCP `compute_vmaf` high-bit-depth input support

## Question

Can the embedded MCP `compute_vmaf` tool close its documented 10/12-bit
YUV gap without widening the whole tool schema to every libvmaf pixel
format?

## Findings

- `compute_vmaf.c` already owns a narrow plain-file reader for YUV420p.
  It hardcoded byte count, picture allocation, and row reads to 8 bpc.
- `vmaf_picture_alloc()` supports `bpc` in `[8, 16]` and doubles the
  row stride for `bpc > 8`.
- The raw YUV convention used elsewhere in this fork stores high-bit
  planar samples as little-endian 16-bit words. Reading
  `width * sample_bytes` into each plane row matches libvmaf's picture
  storage on the supported little-endian CI/runtime hosts.
- YUV422P/YUV444P need a public `pixel_format` argument and schema
  update. Adding them silently would change the MCP tool contract more
  broadly than the backlog item requires.

## Alternatives considered

| Option | Pros | Cons | Decision |
| --- | --- | --- | --- |
| Add optional `bitdepth` only for YUV420p | Closes the HDR/CHUG-adjacent 10/12/16-bit gap; tiny schema change; keeps existing default behaviour | Still no 4:2:2 / 4:4:4 | Chosen |
| Add `pixel_format` plus all planar layouts | Fuller parity with the CLI | Larger schema, more tests/docs, and more room for ambiguous numeric pixfmt names | Deferred |
| Keep 8-bit only | No code change | Leaves the documented v4 roadmap gap open and blocks high-bit-depth MCP scoring | Rejected |

## Decision

Add an optional integer `bitdepth` argument to `compute_vmaf`, accepting
8, 10, 12, or 16 and defaulting to 8. The tool continues to accept
YUV420p only. The result JSON echoes `bitdepth` so clients can audit
which path ran.

## Validation

```bash
meson setup core/build-mcp-hbd libvmaf \
  -Denable_mcp=true -Denable_mcp_stdio=true \
  -Denable_mcp_uds=true -Denable_mcp_sse=enabled
meson test -C core/build-mcp-hbd test_mcp_smoke --print-errorlogs
```

## References

- `req`: user asked to keep taking the next backlog/gap item after the
  docs batch.
- [docs/mcp/embedded.md](../mcp/embedded.md) v4 roadmap row for
  high-bit-depth `compute_vmaf`.
