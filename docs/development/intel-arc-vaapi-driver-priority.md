# Intel Arc + VAAPI driver priority

## TL;DR

If `vainfo` reports `Driver version: VA-API NVDEC driver` on a system
that has both NVIDIA and Intel Arc cards, NVIDIA's NVDEC-VAAPI shim
has shadowed the real iHD driver for Arc. Force the Intel driver
explicitly when you want to use Arc QSV / VAAPI:

```sh
LIBVA_DRIVER_NAME=iHD ffmpeg -init_hw_device qsv:hw,child_device=/dev/dri/renderD129 ...
```

## Why this happens

NVIDIA ships a VA-API shim (`libva-driver-nvidia`) so Chrome / Firefox
can hardware-accelerate decode through their VAAPI codepath without
caring that the underlying driver is CUDA-NVDEC. On a multi-card host
the shim is often picked up first by libva's auto-detection, and any
Arc-targeted call (h264_qsv / h264_vaapi against /dev/dri/renderD129)
then routes through NVIDIA's translation layer instead of Intel's iHD.

Symptom: `Error creating a MFX session: -9. Device creation failed:
-1313558101.` MFX is Intel's Media SDK / oneVPL session protocol —
NVIDIA's shim doesn't speak it, so the handshake fails.

## Fix

Set `LIBVA_DRIVER_NAME=iHD` whenever invoking ffmpeg with QSV /
VAAPI encoders against the Arc card. The fork's
[`scripts/dev/hw_encoder_corpus.py`](../../scripts/dev/hw_encoder_corpus.py)
forces this in its QSV invocation path.

## Verification

```sh
LIBVA_DRIVER_NAME=iHD vainfo --display drm --device /dev/dri/renderD129
# Expect: Driver version: Intel iHD driver for Intel(R) Gen Graphics - 26.x.x
```
