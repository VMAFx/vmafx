- **CI:** the Netflix/vmaf upstream URL is declared once as
  `NETFLIX_VMAF_URL` in `build-config.env`; the pin reader's `--url`
  option is the single runtime reader for the parity script and the
  licence-provenance, upstream-watcher and ffmpeg-integration jobs, so
  no workflow or script repeats the clone URL and an explicit mirror may
  override it without changing Netflix's provenance identity (issue 3098).
