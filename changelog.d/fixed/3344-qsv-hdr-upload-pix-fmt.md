- **HDR10 encodes with `hevc_qsv` Main 10 and `av1_qsv` open again (#3344).**
  The HDR arguments added an output `-pix_fmt p010le` after the `hwupload` filter, so FFmpeg
  tried to convert the uploaded hardware frames back to a software format and the encoder
  failed to open with exit 218. `vmaf-tune` and the Go encode chain now drop an output
  `-pix_fmt` or `-pix_fmt:v` whenever they append an upload filter; chains without an upload
  keep it.
