- **The CAMBI GPU twins copied their device buffers one row at a time, which cost
  more than every other extractor in the pipeline combined.** Each scale stalled
  the stream and then issued two *blocking* row copies per row: about 4,200
  driver round trips per 1080p frame across the five scales. Measured on an RTX
  4090 over 48 frames of 1080p, `cambi_cuda`'s submit took **0.602 s of a 1.03 s
  run** — more than `speed_chroma_cuda`, `adm_cuda` and `motion_cuda` together.
  The copies are now single strided 2D transfers enqueued on the stream with one
  stall for both, taking that phase to **0.142 s** and the whole default-model
  CUDA run from **47 to 67 fps**. Scores are unchanged: CPU and CUDA agree
  exactly on the same clip, and the CUDA suite passes 49/50 (1 skipped). The HIP
  twin carried the identical defect and is fixed the same way; the SYCL twin
  enqueued one copy per row where both sides are packed, so its readback is now
  a single copy.
