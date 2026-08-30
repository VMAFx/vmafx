- **Round-4 audit fixes for the `libvmaf_sycl` FFmpeg filter patch**
  (`ffmpeg-patches/0005`). Two verified defects in the dedicated SYCL filter
  added by patch 0005: (1) **sycl_state leak (high)** — `uninit_sycl` called
  `vmaf_close()` but never `vmaf_sycl_state_free()`; `vmaf_close()` explicitly
  does NOT free the SYCL state (ownership is not transferred), so the SYCL state
  + its USM allocations leaked on every filter close. The free is now called
  explicitly after `vmaf_close()` and the false "vmaf_close handles it" comment
  removed. (2) **QSV NULL deref (med)** — `do_vmaf_sycl` walked the QSV
  zero-copy handle chain (`AVFrame->data[3]` → `mfxFrameSurface1*` →
  `Data.MemId` → `mfxHDLPair*` → `->first`) with no NULL guards; a
  software-fallback QSV surface (no VA backing) crashed the filter. All three
  chain links are now NULL-checked, returning `AVERROR(EINVAL)` with a clear
  diagnostic instead of dereferencing NULL. The full 16-patch series re-applies
  cleanly against `n8.1.1` (`git apply --3way`). (#21 — a cosmetic duplicate
  `check_pkg_config libvmaf_sycl` configure probe — is intentionally left: the
  probe is idempotent, and removing the wrong one risks breaking SYCL
  detection.)
