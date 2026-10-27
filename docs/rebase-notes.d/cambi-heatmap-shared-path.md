## CAMBI heatmaps shared by contexts (2026-10-27)

- `core/src/feature/cambi.c` keeps a locked, reference-counted list of open `heatmaps_path` folders (`HeatmapsPathRef`); `vmaf_cambi_open_heatmaps()` truncates only on a folder's first reference and `vmaf_cambi_close_heatmaps(path, files)` drops one.
- Ported from Netflix/vmaf PR 1707 (issue 1676), which is not merged upstream at the time of the port.
- **On sync:** if upstream merges PR 1707 in another form, take upstream's arithmetic but keep the two-argument close used by the Metal twin (`core/src/feature/metal/integer_cambi_metal.mm`); `test_heatmap_writers_shared_path` in `core/test/test_cambi_heatmap_writers.c` must keep passing.
