- **The SYCL dma-buf import no longer closes the caller's descriptor.**
  `vmaf_sycl_dmabuf_import()` handed the caller's descriptor to Level Zero,
  whose compute runtime 26.35 closes it when the buffer is imported again
  while its first import is alive; `libvmaf_sycl.h` leaves the descriptor to
  the caller, and the VA surface import closed the same number again, which
  could close an unrelated descriptor another thread had opened in between.
  Level Zero now gets a private duplicate, and the caller's descriptor stays
  open on every driver.
