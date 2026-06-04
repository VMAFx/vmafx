- Port upstream Netflix commits `dff4082b` and `46d3a154`: make batch-threaded
  execution the unconditional default in `core/src/libvmaf.c` by removing all
  `#ifdef VMAF_BATCH_THREADING` guards. The fork now matches upstream threading
  semantics; the old per-extractor non-batch thread pool path is removed (ADR-1051).
