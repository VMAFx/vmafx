- CUDA feature-extractor init / submit error paths: free already-acquired
  resources instead of leaking them on partial-init or per-frame failure
  (RC independent-audit findings).
  - `integer_ms_ssim_cuda.c`: `close_fex_cuda` now frees the pinned host
    input buffers (`h_ref` / `h_cmp`) and the per-scale pinned host partials
    triples (`h_l_partials` / `h_c_partials` / `h_s_partials`), which were
    allocated via `vmaf_cuda_buffer_host_alloc` but never released. The two
    `init` `-ENOMEM` returns now route through `close_fex_cuda`, releasing all
    device buffers, the loaded PTX module, and the lifecycle stream/events.
    In `submit`, the `tmp_uint` pinned staging buffer is now freed via a new
    `free_tmp` goto target on the early CUDA-error exits between its
    allocation and its success-path free (previously leaked one staging
    buffer per failed frame).
  - `integer_psnr_hvs_cuda.c`: both `init` bulk-alloc `-ENOMEM` returns now
    route through `close_fex_cuda`, releasing the partially-allocated device
    and pinned-host buffers, the dedicated upload stream + event, and the PTX
    module.
  - `ssimulacra2_cuda.c`: `init` now routes an `ss2c_alloc_buffers` failure
    through `close_fex_cuda`, releasing the partial buffers, the two PTX
    modules, and the stream (previously returned with all three leaked).
  - `speed_chroma_cuda.c`: the `fail_pop` label now performs the same
    module-unload + stream-destroy + buffer-free cleanup as its `fail_after_pop`
    sibling (`free_cuda_buffers` does not touch the module or stream, so both
    labels now unload `s->module` and destroy `s->stream` explicitly before
    freeing the device / host buffers).
