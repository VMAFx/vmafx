- Scaffolding for ADR-0457 offloading of large `model/tiny/*.onnx`
  blobs (>=1 MB) to GitHub Release attachments: the
  `scripts/ai/fetch-tiny-blobs.sh` fetcher script (idempotent
  download + sha256 verification driven by `model/tiny/registry.json`)
  is in place. The actual blob offload is not yet live — the three
  large blobs (`transnet_v2.onnx`, `fastdvdnet_pre.onnx`,
  `lpips_sq.onnx`, totalling ~44 MB) are still inlined in git on
  master pending the `tiny-blobs-v1` GitHub Release upload + matching
  registry update. Until that follow-up lands, the fetcher is a no-op
  on fresh clones (the blobs are already present).
