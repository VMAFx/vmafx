## perf(vif): shrink log2 LUT from 128 KB to 64 KB; cache Gaussian filters per init

The integer VIF `log2_table` was `uint16_t[65537]` (128 KB); after CLZ-based
normalization the mantissa index is always in `[32768..65535]`, so entries `[0..32767]`
were unreachable. Reindexing with `& 0x7FFF` reduces the table to 32768 entries (64 KB),
halving L2 cache pressure on the three AVX-512 gather sites in `vif_statistic_avx512`.

The float VIF Gaussian filter was computed via `expf` four times per frame; it is now
pre-computed once in `VifState.init()` and cached, eliminating 34 transcendental calls
per frame.

Measured speedup: integer VIF 1920×1080 +4.2 FPS (+3.9 ms/frame).

See ADR-0500.
