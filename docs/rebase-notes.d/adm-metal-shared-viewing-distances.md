## ADM second viewing distance on Metal (2026-10-09)

- `core/src/feature/metal/integer_adm_metal_host.{c,h}`: new
  `iadm_metal_view_stages()`, the stage plan of one viewing distance (the
  DWT only for the first).
- `core/src/feature/metal/integer_adm_metal.mm`: `adm_norm_view_dist_extra`
  (`nvde`) in the option table; `iadm_options()` takes the distance; a
  second set of reduction buffers (`accum_x`); `submit` encodes each scale's
  distances in turn and `collect` concludes each. The merge callback and the
  second distance's names are the shared `adm_view_dist.c`. Fork-only,
  nothing upstream to merge. **On sync**: keep the per-distance loop and the
  view stage plan.
- Tests: `test_metal_integer_adm_host_replay.c` replays two distances;
  `test_metal_integer_adm_parity.c` (device and self-test) gains
  `test_adm_two_views_exact` and `test_adm_merged_registrations_exact`;
  the Metal option-table contract drops its recorded gap.
