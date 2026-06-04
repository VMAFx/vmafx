### HIP + Metal kernel correctness fixes (ADR-1030)

Three HIGH-severity GPU backend correctness defects fixed:

- **HIP adm_decouple**: `#include "adm_decouple_inline.hip"` added after
  `common.h`; removes a bare dangling function body that made the TU
  invalid C++ and left `get_best15_from32` undefined for the scale-1..3
  decouple path
  (`core/src/feature/hip/integer_adm/adm_decouple.hip`).

- **HIP wavefront_reduce_i64**: reassembly changed from bitwise OR to integer
  addition — `(int64_t)((uint64_t)lo + ((uint64_t)hi << 32))` — so carry
  from the lo-half lane-sum is preserved when `num_non_log` or other large
  VIF accumulators exceed 2^32 total across 64 wavefront lanes
  (`core/src/feature/hip/integer_vif/vif_statistics.hip`).

- **Metal float_motion vertical halo**: `TILE_H` increased from 16 to 20 and
  `wg_oy` changed to `bid.y * 16 - HALF_FW` in both the tile-load and
  vertical-filter phases for the 8bpc and 16bpc kernels; horizontal-filter
  `ty` offset corrected to `lid2.y + HALF_FW`. Mirrors the already-correct
  pattern in `integer_motion.metal`. Without the halo, every workgroup except
  the first row had 2 corrupted blurred rows per frame, producing wrong SAD
  scores (`core/src/feature/metal/float_motion.metal`).
