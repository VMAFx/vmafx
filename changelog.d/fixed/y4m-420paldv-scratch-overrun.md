- **Heap buffer overflow parsing any `.y4m` file that declares
  `C420paldv`.** `y4m_convert_42xpaldv_42xjpeg()` runs a horizontal
  filter into a scratch area carved out of `aux_buf`, and the vertical
  filters rewind by `c_sz` to read it back — so the scratch is
  per-plane and must be reused by the second chroma plane. It was not:
  `tmp` ran on from where plane 1 left it, so plane 2 wrote the whole
  of its scratch past the end of the allocation. `aux_buf_sz` is
  `3 * c_sz` (two source chroma planes plus one scratch); the
  running pointer needs `4 * c_sz`. Upstream indexes a fixed `tmp`
  base rather than advancing it, which is why upstream's sizing is
  correct there — this fork's extraction of `y4m_horizontal_filter_row`
  turned that base into a running pointer and lost the reuse. A 4x4
  frame overflows by 1 byte (ASan: `WRITE of size 1` past a 12-byte
  region); the overrun grows with the picture. Reachable from any
  untrusted `.y4m` input, so this is a write-what-where from a file
  format the CLI opens by default. Found by libFuzzer + ASan;
  reproducer promoted to
  `core/test/fuzz/y4m_input_corpus/y4m_420paldv_scratch_overrun.y4m`.
