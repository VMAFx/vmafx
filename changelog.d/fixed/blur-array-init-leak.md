- **`init_blur_array` partial-allocation leak.**
  `core/src/feature/common/blur_array.c::init_blur_array` allocated
  per-entry blur buffers in a loop and returned `0` immediately when
  any `aligned_malloc` failed mid-loop — leaving previously-allocated
  entries `[0 .. i-1]` with live `aligned_malloc` pointers that the
  caller never freed. Same partial-allocation leak class as the
  SYCL (PR #293), HIP (PR #290), and CUDA (PR #289) init paths
  fixed today. Fix: on failure, `aligned_free` each previously-
  allocated entry and reset `actual_length = 0` before returning.
  Identified by c-reviewer agent audit 2026-05-30 (MEDIUM severity).
