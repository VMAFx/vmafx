The HIP backend now includes a fully functional `integer_motion_v2_hip`
feature extractor. The implementation mirrors `integer_motion_v2_cuda`
call-graph-for-call-graph: ping-pong device buffers, 8bpc and 16bpc
`hipModuleLaunchKernel` dispatch via a precompiled HSACO blob, and a
host-side `flush()` computing `motion2_v2 = min(cur, next)` with
optional `motion_fps_weight` correction. Arithmetic right-shift
correctness is preserved per ADR-0138/0139.
