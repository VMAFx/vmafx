- **`vmaf-tune corpus --auto-hdr / --force-sdr / --force-hdr-pq /
  --force-hdr-hlg` CLI flags
  ([ADR-0300](../docs/adr/0300-vmaf-tune-hdr-aware.md)).** Surfaces
  the HDR-mode plumbing on the corpus subparser; threads the choice
  through to `CorpusOptions.hdr_mode` so downstream rows can be
  tagged. The actual `iter_rows` integration (per-source ffprobe
  detection + codec-arg injection + HDR-VMAF model selection) lands
  in a follow-up PR — `hdr.py` already exposes `detect_hdr`,
  `hdr_codec_args`, and `select_hdr_vmaf_model` from the earlier
  Bucket #9 module merge.
