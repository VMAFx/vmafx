# vmaf-tune: QSV/AMF hardware-device init + probe size (ADR-0601)

Three bugs in `vmaf-tune compare` that blocked BBB v14 hardware-encoder runs
are now fixed:

- **Probe resolution (V14-A):** `probe_encoder_available()` now issues the
  dummy encode against a `320×240 @ 24 fps, 0.5 s` source instead of the
  previous `64×64 @ 1 fps, 0.04 s`. The old resolution was below NVENC's
  hardware minimum (~145×49) and QSV's minimum (~128×96), causing every
  hardware encoder to fail the probe with `EINVAL` even on hosts with a
  fully-working GPU. 320×240 clears all known hardware minima.

- **QSV VA-API device init (V14-B):** Intel QSV encodes now receive the
  required FFmpeg hardware-device initialisation chain before the input flag:
  `-init_hw_device vaapi=va:<dev> -init_hw_device qsv=qsv_dev@va
  -filter_hw_device va`, plus `-vf format=nv12,hwupload=extra_hw_frames=64`
  before the encoder. Without these flags every QSV encode (probe and
  production) fails with `-22 Invalid argument`. The VA-API render node is
  `/dev/dri/renderD128` by default, overridable via `--vaapi-device PATH` or
  `VMAFTUNE_VAAPI_DEVICE` env var for mixed-GPU hosts.

- **AMF gfx1036 documentation (V14-C):** The AMD Raphael / Phoenix APU iGPU
  (gfx1036) contains a VCN decode block but no VCE encode block. AMF encoding
  on this silicon fails with `AMF_NOT_SUPPORTED` regardless of driver or
  runtime state. The `_amf_common.py` module docstring now documents this
  hardware limitation so operators are not surprised when the probe correctly
  returns `(False, "dummy encode failed")` on Ryzen 7000 iGPU hosts.
