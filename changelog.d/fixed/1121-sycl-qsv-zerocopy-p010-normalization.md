- `libvmaf_sycl` zero-copy filter no longer returns `VMAF score: nan` with
  garbage `integer_motion` on QSV-decoded 10/12-bit pairs (ADR-1121). Two root
  causes were fixed: (1) imported P010/P012 luma is now normalized MSB→LSB
  (`>> (16 − bpc)`) in the SYCL import to match the value range the CPU path
  gets via FFmpeg's `P010LE→YUV420P10LE` conversion — previously raw
  MSB-aligned pixels inflated `integer_motion` by 64× and NaN-ed ADM/VIF; and
  (2) the supported invocation now requires a **separate `-init_hw_device qsv=…`
  per decoder** so the two decoders do not share one VA surface pool (a shared
  pool let the reference decoder overwrite the distorted surface). With both in
  place the zero-copy path matches the CPU oracle to three significant figures
  (0 NaN). The zero-copy path now also **defaults to direct SYCL dispatch**
  instead of the combined graph (byte-identical output, ~15–25 % faster at 4K on
  Intel Arc); users no longer need the deprecated `VMAF_SYCL_NO_GRAPH=1`, and an
  explicit `VMAF_SYCL_USE_GRAPH=1` still forces the graph. Adds the
  `VMAF_SYCL_IMPORT_DEBUG=1` diagnostic env var. Netflix CPU golden assertions
  are unchanged. See
  [ADR-1121](../docs/adr/1121-sycl-qsv-zerocopy-p010-normalization.md) and the
  [SYCL backend doc](../docs/backends/sycl/overview.md).
