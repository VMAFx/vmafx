- **clang-tidy measures the GStreamer element and the FFmpeg filter sources (ADR-3379).**
  A new tidy lane, `integrations`, builds `gstreamer/` and the patched FFmpeg
  tree in the dev container and runs the ratchet on `gstreamer/src/*.c` and
  `ffmpeg-patches/src/*.c`; it is the required check `Tidy Lane (integrations)`,
  routed by its own paths, and part of the nightly sweep (#2860).
