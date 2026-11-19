- **CI:** the Netflix/vmaf upstream URL is declared once as
  `NETFLIX_VMAF_URL` in `build-config.env`; the parity script and the
  licence-provenance, upstream-watcher and ffmpeg-integration jobs now
  consume that shared default, which an explicit mirror may override
  without changing Netflix's provenance identity (issue 3098).
