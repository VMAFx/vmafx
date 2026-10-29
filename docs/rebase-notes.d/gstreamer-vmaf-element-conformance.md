## GStreamer vmaf element against Netflix's libvmaf (2026-10-28)

- `scripts/ci/upstream-gstreamer-compat.sh`: compiles the element against the
  `--reference-prefix` headers and runs it on both libraries; adds
  `--planted-defect`. Fork-only, nothing upstream to merge.
- `scripts/ci/upstream_consumer_scores.py`: XML and CSV readers.
- `.github/workflows/ffmpeg-integration.yml`: builds Netflix's libvmaf at
  the pin `scripts/ci/upstream_parity_pin.py` prints. **On sync**: moving the
  pin in `docs/development/known-upstream-bugs.md` moves this build too; a pin
  whose `VmafPicture` differs from the fork's needs no change here, because
  the element is built against Netflix's headers.
