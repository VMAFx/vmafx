`psnr_hvs_cuda` now joins the CUDA engine-scope drain batch: plane
partial readbacks are queued during submit, the lifecycle is registered
with `drain_batch`, and collect performs host-side reduction after the
batched wait instead of issuing its own per-extractor stream sync.
