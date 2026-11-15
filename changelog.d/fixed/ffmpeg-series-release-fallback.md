- **The shared FFmpeg fix series fetches a failed github.com release download via the forge API.**
  `scripts/ci/ffmpeg-shared-series.sh` fetches a github.com release asset again with
  `gh release download` (forge API) when curl or wget fails on it, as it did with HTTP 504 for
  anonymous traffic on 2026-11-09. The sha256, `SHA256SUMS` and signature checks run unchanged
  after either path, and the output names the path that ran.
