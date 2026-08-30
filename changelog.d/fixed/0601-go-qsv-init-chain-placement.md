- Fixed the Go encoder's Intel QSV device-init chain landing in the wrong
  ffmpeg argv position (`pkg/encoder/hardware.go`). `injectQSVInitChain`
  appended `-init_hw_device vaapi=… -init_hw_device qsv=… -filter_hw_device va`
  to `EncodeParams.ExtraArgs`, which the encode helper emits *after* `-c:v`.
  ffmpeg accepts those only as global options before the first `-i` and
  otherwise fails the encode with `-22 Invalid argument`, so every `h264_qsv` /
  `hevc_qsv` encode driven through `pkg/encoder` — `compare`, `ladder` and now
  `tune-per-shot` — failed on a host with a working Intel driver. The
  device-init flags now go to the new `EncodeParams.InputArgs` field (emitted
  before `-i`) while the `-vf format=nv12,hwupload=extra_hw_frames=64` filter, a
  per-output option, stays in `ExtraArgs`. This matches the split the Python
  `compare.hw_device_init_args` / `_qsv_common.hw_device_init_args` pair already
  made (ADR-0601). The two `injectQSVInitChain` unit tests were asserting the
  broken placement and now pin the corrected one.
