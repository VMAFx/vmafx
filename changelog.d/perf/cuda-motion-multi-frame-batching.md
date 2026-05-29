### CUDA motion extractor: N-slot SAD ring buffer (ADR-0766)

Replace the single-slot `VmafCudaBuffer *sad` with an N-slot device ring
(`sad_ring`, N=16 default) and a matching N-element pinned host array
(`sad_host`).  Each frame now uses an independent accumulator slot
(`slot = (index-1) % N`), eliminating the false-dependency between
consecutive frames that the shared single-slot design had.  The ring
provides the correct layout for a planned future engine-level N-frame
dispatch optimisation that will amortise `cuStreamSynchronize` overhead
across N frames (projected 576p fps improvement: ~79 → ≥800).

Configure at build time: `-Dmotion_batch_n=N` (integer 1–64, default 16).
Requires `enable_cuda=true`.  Per-frame scores are unchanged;
ADR-0214 `places=4` cross-backend gate continues to pass.
