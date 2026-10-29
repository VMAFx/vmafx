## Odd-depth device twins: allow-list (2026-10-28)

[ADR-3383](adr/3383-odd-depth-twin-allow-list.md), #2476.
`refuse_unscaled_bpc()` in `core/src/feature/feature_extractor.cpp` is an
allow-list of exact twin names with a maximum depth each (`kMeasured`: the CUDA,
SYCL and HIP `float_psnr` and `ciede` twins up to 15 bits, the `psnr_hvs` twins
of CUDA, SYCL, HIP and Metal up to 11 bits): every other device twin is refused
at 9, 11, 13, 14 and 15 bits. A sync must not bring back the deny-list or a
prefix match; a twin joins the table by exact name only with those depths in
its parity test. `test_read_pictures_bpc` guards both sides.
