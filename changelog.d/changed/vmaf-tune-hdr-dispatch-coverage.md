`vmaf-tune` HDR mode now emits central color-signaling args for AV1
NVENC, HEVC/AV1 QSV, HEVC/AV1 AMF, HEVC VideoToolbox, and libaom-av1
instead of leaving those HDR-capable cells with empty `hdr_args`.
