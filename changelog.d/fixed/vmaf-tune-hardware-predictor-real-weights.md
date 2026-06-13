Replace six `vmaf-tune` hardware predictor stubs with real-corpus
weights trained from the Phase-A hardware sweep: `h264_nvenc`,
`hevc_nvenc`, `av1_nvenc`, `h264_qsv`, `hevc_qsv`, and `av1_qsv`.
The predictor trainer now consumes historical hardware-sweep aliases
(`codec` / `q` / `vmaf` / `actual_kbps`) directly.
