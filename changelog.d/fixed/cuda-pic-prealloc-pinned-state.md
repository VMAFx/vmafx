- **CUDA: `test_cuda_runtime_unwind` pins allocating state on host-pinned pictures.**
  Host-pinned pictures (`vmaf_cuda_picture_alloc_pinned`) record the allocating
  state on `priv->cuda.state`, preventing a NULL dereference of `state->f` during
  unref (upstream Netflix/vmaf#1573 hunk a). A device-free test
  `test_pinned_picture_release_uses_the_allocating_state` exercises the allocation
  and unref through the fake driver table, ensuring the allocating state is pinned
  across platforms.
