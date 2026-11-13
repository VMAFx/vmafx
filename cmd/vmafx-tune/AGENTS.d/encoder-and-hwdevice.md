---
paths:
  - pkg/encoder/*.go
  - pkg/hwdevice/*.go
  - pkg/ffencode/*.go
  - pkg/codecadapter/*.go
invariant: Absolute vs Quality CRF windows; pre-input InputArgs vs post-input ExtraArgs; QSV/AMF argv parity.
---
<!-- markdownlint-disable MD013 MD029 -->
# Encoder scope, CRF windows, ffmpeg arg placement and QSV/AMF parity

## Rebase-sensitive invariants

5. **Stage-1 scope** (`pkg/encoder/encoder.go`): `encoder.New` accepts only
   `libx264` and `libx265`. Hardware encoders (NVENC, QSV, AMF) and SVT-AV1 =
   Stage-2 scope. Do not add hardware encoder support here without new ADR and
   associated hw-init flag plumbing from Python `compare.py`.

14. **Two quality windows per codec, are not interchangeable**
    (`pkg/encoder/adapter.go`): `Adapter.AbsoluteLo/Hi` = window CRF
    bisect **searches** (ADR-0538 — wide, so premium-archival VMAF targets
    stay reachable), while `Adapter.QualityLo/Hi` = informative window
    per-shot tuner **clamps final recommendation into**. Differ
    for `libx265` (0..51 vs 15..40) and `libsvtav1` (0..63 vs 20..50).
    `AdapterEncoder.CRFRange()` deliberately returns *absolute* pair
    because `bisect.Run` consumes it as search domain. Collapsing two
    changes which CRFs tuner can reach.

15. **`EncodeParams.InputArgs` vs `ExtraArgs` = placement contract**
    (`pkg/encoder/encoder.go`): `InputArgs` emitted **before** `-i`,
    `ExtraArgs` **after** `-c:v`. ffmpeg rejects demuxer options
    (`-f rawvideo -pix_fmt -s -r`) and device-init options
    (`-init_hw_device`, `-filter_hw_device`) anywhere but pre-input
    position — QSV chain failed with `-22 Invalid argument` for exactly
    this reason until split existed (ADR-0601). Filter options (`-vf`)
    must stay post-input. Do not "simplify" two fields into one.

24. **Every QSV encode carries device chain, in both implementations**
    (`pkg/hwdevice`, `pkg/ffencode` `BuildFFmpegCommand`, `pkg/encoder`
    `injectQSVInitChain`; Python `vmaftune.encode.build_ffmpeg_command`):
    `-init_hw_device vaapi=va:<node> -init_hw_device qsv=qsv_dev@va
    -filter_hw_device qsv_dev` before first `-i`, and
    `format=nv12,hwupload=extra_hw_frames=64` (8-bit) or
    `format=p010le,hwupload=extra_hw_frames=64` (10-bit) appended to request's own
    `-vf` chain (ADR-0601). filter device is QSV device: with
    `-filter_hw_device va` upload produced `vaapi` frames and filter
    graph failed before encoder opened. render node resolves through
    `hwdevice.ResolveVAAPIDevice` (explicit, `VMAFTUNE_VAAPI_DEVICE`, first
    Intel node, `/dev/dri/renderD128`), Python resolver's order. Change
    chain in both implementations at once; `encode-profile --dry-run`
    argv must stay identical to Python's.
    Upload follows output depth: `hwdevice.QSVUploadFilter` / Python
    `_qsv_common.qsv_upload_filter`; output `-pix_fmt` overrides source format,
    `p010le`, `yuv420p10le`, or `-profile:v main10` selects P010, HDR args included.
    Legacy upload constants stay NV12.

25. **AMF adapters emit constant-QP block once** (`pkg/codecadapter`):
    Python AMF `extra_params()` no longer repeats `-quality/-rc/
    -qp_i/-qp_p` block `ffmpeg_codec_args` emits, so Python and Go argv agree
    byte for byte (pkg/codecadapter `AGENTS.md` invariant 3). Repeating it on
    either side is parity break, not inert duplicate.
