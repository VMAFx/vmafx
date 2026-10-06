---
paths:
  - core/src/feature/sycl/integer_motion_sycl.cpp
  - core/src/feature/sycl/integer_motion_v2_sycl.cpp
  - core/test/test_sycl_motion_five_frame_window.c
invariant: SYCL motion twins keep n-2 outside the graph; CPU window function per frame (advance) and at flush; no new kernel.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# `motion_five_frame_window` on `motion_sycl` / `motion_v2_sycl` (ADR-1491)

- CPU contract (ADR-1478, Netflix `a2b59b77`): SAD of frame n against frame
  n-2, 0 for n < 2; `motion2` / `motion3` from
  `vmaf_motion_window_flush()` (`core/src/feature/motion_window.h`).
- No kernel added or changed: `motion_sycl_pipeline::enqueue_sad()` takes
  `prev` as an argument. Scratch ratchet (ADR-1395), sub-group sizes
  (ADR-1468), AOT targets untouched.
- `motion_v2_sycl` (own queue, no graph): ring of `ring = five ? 3 : 2`
  planes; frame n copies into `d_pix[n % ring]`, kernel reads
  `d_pix[(n + 1) % ring]`; SAD from frame `ring - 1` on.
- `motion_sycl` (combined command graph, two recorded slots): option on ->
  `d_raw_y[0]` = frame n-2, `d_raw_y[1]` = frame n-1, fixed roles.
  `enqueue_motion_work()` enqueues the kernel on **every** frame with
  `cur_copy = nullptr`: graph is recorded once (second frame) and replayed,
  a per-frame `has_prev` would be frozen at record time. Frames 0 / 1: SAD
  computed on zeroed planes, not read (`motion_score_from_sad()` min_idx 2).
  `motion_post_graph()` (plain queue commands behind the replay's barrier):
  read back SAD from frame 2 on, then `d_raw_y[0] <- d_raw_y[1]`,
  `d_raw_y[1] <- shared ref`. Never move those copies into the recorded
  graph, never key the planes on the graph slot (slot parity != frame parity
  is silent three-frame window).
- `motion_sycl`: option + `motion_add_uv` -> `-ENOTSUP` (no CPU reference
  for chroma).
- Host, `motion_sycl`, option on: `collect()` stores SAD (+ debug `motion`)
  only; `advance_fex_sycl()` (`.advance`, engine calls it per frame) =
  `vmaf_motion_window_advance()`, `flush()` -> `motion_flush_window()` for the
  rest, both on `s->window_state` (ADR-2090). Three-frame path and
  `motion_force_zero`: advance returns 0. `motion_v2_sycl`: `.advance` =
  `vmaf_motion_window_advance()`, `flush()` = `vmaf_motion_window_flush()`,
  both windows; no local copy of the CPU derivation.
- Guards: `test_sycl_motion_five_frame_window` (`==`, six option sets, 11 / 1
  / 2 / 3 frames, 8 / 10 bit; fixture
  `core/test/motion_five_frame_twin_parity.h`; frame i final before flush by
  read `max(i + 1, 2) + 1`, `lag_motion*` = 2), `test_sycl_twin_option_parity`,
  `test_motion_window_advance_contract.py`, gate cells `motion_mffw`,
  `motion_v2_mffw` (exact).
