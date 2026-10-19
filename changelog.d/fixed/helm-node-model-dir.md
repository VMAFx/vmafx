- **Helm: a node without a models volume scores with the image's models.**
  The node Deployment pointed `VMAFX_MODEL_DIR` at `persistence.models.mountPath`
  even when no models volume was mounted (the default), so every job failed
  with "model not found". It now uses the mount path only with
  `persistence.models.enabled` and `/usr/local/share/vmafx/model` otherwise.
