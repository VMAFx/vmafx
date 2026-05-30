- **`compat/python-vmaf/` coverage uplift — 55 focused unit tests for
  fork-touched config plumbing and previously-untested leaf
  utilities.** New file
  [`python/test/compat_python_vmaf_coverage_test.py`](../python/test/compat_python_vmaf_coverage_test.py)
  lifts coverage on `config.py` (53% → 88%, exercises
  `download_reactively` happy / partial-cleanup / HTTPError /
  cache-hit paths plus all `VmafConfig.*_path` joiners and
  `VmafExternalConfig.get_and_assert_*` validators),
  `tools/stats.py` (0% → 84%, `moving_average` simple / exponential /
  unknown-type, `nonemean` NaN edge, `print_stats`),
  `tools/writer.py` (0% → 90%, `YuvWriter` round-trip for `yuv420p`,
  `gray`, `yuv420p10le` plus context-manager + assertion paths),
  `tools/convex_hull.py` (20% → 95%, empty / single / duplicate /
  non-monotonic / 4-point lower-hull), `tools/interpolation_utils.py`
  (51% → 100%, all three `pchipend` branches + `computeRate`
  polynomial seam + 4-point PCHIP pipeline),
  `tools/decorator.py` (35% → 55%, `deprecated` /
  `dummy` / `memoized` / `override` / `change_repr`),
  `tools/exceptions.py` (88% → 100%) and `tools/typing_utils.py`
  (88% → 100%). No Netflix golden assertions touched
  ([CLAUDE.md §8](../CLAUDE.md)); no `vmaf` binary subprocess
  exercised (the lifted modules are pure-Python leaves). Total
  `compat/python-vmaf/` line coverage from the fast-pure-Python
  pytest subset rises from 16% to 18%.
