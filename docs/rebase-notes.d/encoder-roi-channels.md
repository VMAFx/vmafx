## Saliency ROI channels follow measured encoder options (2026-10-28)

- `pkg/saliency` and `tools/vmaf-tune/src/vmaftune/saliency.py` send libsvtav1 an x264-style 16x16 macroblock qpfile with suffix `.svtav1-qpfile.txt` through patch 0007's top-level `-qpfile` option. The bridge reduces it to 64x64 superblocks with at most eight segments (ADR-3386).
- Keep libvvenc out of saliency dispatch and supported-encoder lists: the measured wrapper refuses `ROIFile`, and no patched ROI-map channel exists. Its Python adapter declares `supports_saliency_roi = False`; SVT-AV1 declares `supports_qpfile = True`.
- Retain the legacy SVT-AV1 and VVenC public formatters, argv helpers and constants for API stability. They must remain unused by encode paths. Executor ROI detection recognises `-qpfile` and `zones=`, not the rejected `qp-file=` or `ROIFile=` keys.
- On sync, preserve the Go qpfile body/argv and refusal tests, Python saliency dispatch and cleanup tests, and executor ROI-reporting tests. P4-7 and P4-12 of #2477 remain open.
