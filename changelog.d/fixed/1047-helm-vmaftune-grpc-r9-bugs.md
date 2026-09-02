- Helm chart: add missing `storage` key to `values.yaml` so `storage.mode=rclone`
  deployments are not silently undefined (ADR-1047).
- Helm schema: add `networkPolicy`, `auth`, and `otelCollector` to
  `values.schema.json` so typos in those blocks surface at `helm lint` time
  rather than being silently accepted (ADR-1047).
- Helm schema: raise `gpu.count` minimum from 0 to 1 and add `gpu.enabled`
  to the `gpu` object's `required` array; requesting 0 GPU units with a
  vendor device plugin is a silent no-op (ADR-1047).
- vmaf-tune: add `"duration_s"` to `_stamp_tracked_default_sentinels` so the
  `ladder` sub-command's `--duration` flag (dest=`duration_s`) is correctly
  detected as user-provided vs default (ADR-1048).
- vmafx-node: replace fixed 10s retry interval in the online-feedback
  `drainLoop` with exponential backoff (base 2s → cap 2m, reset on successful
  connection) to reduce log noise during extended sidecar outages (ADR-1049).
