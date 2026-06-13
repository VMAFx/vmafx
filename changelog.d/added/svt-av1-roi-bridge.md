- **`ffmpeg-patches/0007` SVT-AV1 ROI bridge — full impl**:
  patch 0007's libsvtav1 hook is no longer scaffold-only. It now
  sets `enc_params.enable_roi_map = true`, builds one
  `SvtAv1RoiMapEvt` per qpfile frame upfront (per-MB qp_offsets
  averaged into a per-64×64-SB `b64_seg_map` of up to 8 segment
  QPs; uniform binning when the QP-offset value span exceeds the
  segment budget), and attaches each event as a `ROI_MAP_EVENT`
  priv-data node on every `eb_send_frame()` with
  `node->size = sizeof(SvtAv1RoiMapEvt*)` per SVT-AV1's
  `resource_coordination_process.c` validation contract. Events
  and segment maps live for the entire encode session because
  SVT-AV1 reads `ROI_MAP_EVENT` data via shallow-copied pointers
  on async pipeline threads (per `enc_handle.c::copy_private_data_list`).
  Wiring is gated on `SVT_AV1_CHECK_VERSION(1, 6, 0)`; older
  SVT-AV1 builds keep the log-and-continue fallback. Smoke:
  `ffmpeg -f lavfi -i testsrc2=size=128x128:r=10:d=0.5 -c:v libsvtav1
  -qpfile clip.qpfile -f null -` against SVT-AV1 v4.1.0 logs
  `ROI bridge enabled.` and encodes clean (was:
  `Svt[error]: invalid private data of type ROI_MAP_EVENT` in the
  scaffold). 9/9 patches still apply against pristine n8.1 via
  `git am --3way`. libaom-av1's `AV1E_SET_ROI_MAP` bridge stays
  deferred to a separate follow-up. Retires the SVT-AV1 deferral
  noted in ADR-0312; no new ADR.
