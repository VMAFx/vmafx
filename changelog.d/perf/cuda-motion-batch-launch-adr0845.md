## CUDA motion: 8-frame SAD batching to amortise per-launch overhead (ADR-0845)

The CUDA `integer_motion` feature extractor previously issued one `cuStreamSynchronize`
per frame, making it dispatch-bottlenecked at all resolutions below 4K
(Research-0760: 0.22× CPU at 576p). Kernel execution was only 0.1% of total
wall time.

`submit()` now queues kernel launches into per-slot device SAD buffers without
triggering a device-to-host readback. `collect()` defers the readback and score
emission to every 8th frame (MOTION_BATCH_DEPTH=8), issuing a single
`cuStreamSynchronize` to drain all 8 pending copies at once. `flush()` handles
the final partial batch. The change is transparent to callers; correctness is
preserved at ADR-0214 places=4 tolerance.

Expected throughput improvement: 576p from ~79 fps to ~800 fps (CUDA/CPU ratio
0.22× → >2×); 1080p comparable; 4K unchanged.
