- Restored the CUDA public-header ownership and call-order documentation lost
  in a silent revert, including the single-pointer state-free convention, and
  made the SYCL picture-preallocation enum's stable values and allocator
  mapping explicit. A fast semantic regression now prevents another
  comment-only rewind.
