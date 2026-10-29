## QSV upload format follows the output depth (2026-10-28)

- `hwdevice.QSVUploadFilter()` (Go) and `qsv_upload_filter()` (Python, `codec_adapters/_qsv_common.py`) are the one place each language picks the QSV upload format: the last `-pix_fmt` wins, `p010le` / `yuv420p10le` / Main 10 give `format=p010le,hwupload=extra_hw_frames=64`, anything else the `nv12` chain. `ffencode`, `encoder.injectQSVInitChain`, `vmaftune.encode` and `scripts/dev/hw_encoder_corpus.py` call them; a rebase must not bring back a fixed `format=nv12` upload in those callers.
- The QSV adapter version is 3 in both registries (`pkg/codecadapter`, `BaseQsvAdapter.adapter_version`); move both together.
