# VMAFxMetricsReadErrors

**Meaning.** For 15 minutes, every scrape of the instance in the labels failed
to read the values of `source`: `queue` (the controller's job queue) or
`device_memory` (a node's GPU memory). Those series are missing from that
`/metrics` page; the rest of the page is served
(`vmafx_metrics_read_errors_total`).

**Impact.** For `queue`, the pending, running and oldest-job series and the
queue alerts go blind. For `device_memory`, the node's GPU memory panels stay
empty.

## Diagnose

- `queue`: the controller could not read its SQLite queue within 2 seconds.
  Check the controller log for database errors and the volume of
  `VMAFX_DB_PATH` for space and I/O latency.
- `device_memory` on a CUDA node: `nvidia-smi` is missing or cannot reach the
  driver. Run `kubectl exec <node-pod> -- nvidia-smi`; in Kubernetes the
  NVIDIA container toolkit mounts it only when `NVIDIA_DRIVER_CAPABILITIES`
  includes `utility`.
- `device_memory` on a HIP node: the amdgpu sysfs files
  (`/sys/class/drm/card*/device/mem_info_vram_*`) are not readable in the
  container.

## Fix

Repair the queue volume, or give the node container `nvidia-smi` (driver
capability `utility`) or read access to the DRM sysfs entries. The alert
clears 10 minutes after reads succeed again.

Dashboard: Nodes and devices (_Device memory reads failing_).
