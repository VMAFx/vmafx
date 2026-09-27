- The `vmafx-node` image bundles rclone at `/usr/local/bin/rclone` again, as the
  storage guide states. The node resolves `s3://`, `gs://`, `rclone://` and
  `remote:path` inputs by running rclone (ADR-0719), but `docker/Dockerfile.node`
  never installed it, so every remote input failed. The static binary comes
  from the official rclone image, pinned by digest in `build-config.env`, and
  the release smoke test now runs it.
