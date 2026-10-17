- **ROCm container layer downloads retry the entire pipeline.** Mid-stream network
  resets during layer extraction previously failed with curl exit 23 because
  curl's in-pipe `--retry` restarted into a broken pipe; the installer now drops
  curl's in-pipe retry and retries the full download-decompression-tar pipeline.
