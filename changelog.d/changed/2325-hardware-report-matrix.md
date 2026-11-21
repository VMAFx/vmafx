- The hardware reports page now lists one row per measured backend (CPU, Metal,
  CUDA, HIP or SYCL device) with its own exactness and result, filters rows by
  device text, backend, exactness, result and operating system in the browser,
  and keeps the newest report when the same device and backend repeat on the
  same machine, linking the older report from the newest row. A device's
  Exactness is its twin-measurement status, its Result fails when the device or
  its state rows fail, a Metal row also fails when the Metal gate fails, and a
  second card of the same model in one report keeps its own row.
