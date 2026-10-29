## Direct y4m reader: shallower stream left-shifted to the picture's depth (2026-10-27)

- Port of Netflix/vmaf PR 1682 (open upstream at the time of the port).
  `y4m_fetch_into_vmaf_picture()` in `core/tools/y4m_input.c` hands a stream
  whose depth differs from the picture's to `y4m_fetch_upshifted()`.
- Fork difference from the upstream diff: `y4m_visible_planes()` moves each
  plane from the padded frame origin that `y4m_input_fetch_frame()` returns to
  the visible picture before the shift; without it the shift reads padding.
- **On sync**: when upstream merges PR 1682, keep `y4m_visible_planes()` and
  the refusal of a deeper stream or an 8-bit picture; `test_y4m_mixed_depth`
  guards both.
