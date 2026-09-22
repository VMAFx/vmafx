- GPU-lane NOLINT sweep (BUG-029) and GPU tidy-baseline re-measure
  (BUG-041). The ADR-0141 §2 citation rule had only ever been swept
  on the CPU lane (PR #388 / ADR-0278); the GPU lane still carried
  21 `NOLINT` markers with prose justifications but no inline
  `ADR-NNNN` token, which `scripts/ci/tidy-ratchet.py`
  `count_uncited_nolints()` counts as uncited. Each of the 21 was
  checked against a real clang-tidy run rather than cited on faith:
  **seven were dead** — the suppressed check produced no diagnostic
  at all — and are removed rather than cited
  (`ssimulacra2_cuda.c` `ss2c_setup_gaussian` function-size, one
  `bugprone-branch-clone` and five `misc-const-correctness` markers
  across `integer_adm_sycl.cpp` / `integer_motion_sycl.cpp`, whose
  comments claimed the analyzer "cannot trace" an `atomic_ref`
  mutation it in fact traces correctly). **One was live but
  misdescribed**: the `misc-unused-parameters` marker on
  `launch_dwt_hori_pair` named `buf_stride`, which is used, while
  the genuinely unused parameter was `h_add`; the horizontal DWT
  pass derives the identical rounding addend locally from `h_shift`
  (`1 << (h_shift - 1)` = 32768 / 16384, matching every row of the
  shift table), so `h_add` and its feeding struct field are removed
  instead of suppressed. Output is unchanged. The remaining 13 are
  load-bearing and now carry the ADR that forces them — ADR-0141 §2
  for the CUDA Driver-API `performance-no-int-to-ptr` casts, the
  SYCL `extern "C"` entry-point linkage brackets and the Metal
  scalar-diff parity port; ADR-0141 §2 + ADR-0278 for the two
  `test_sycl_motion3_parity.c` harness functions, matching the
  wording their three already-cited siblings in the same file use;
  and ADR-1264 for the HIP `-ENOSYS` scaffold skip contract.
  Tree-wide uncited NOLINT count: 21 → 0.
- The SYCL clang-tidy lane measured **zero** SYCL feature translation
  units. meson emits them as `CUSTOM_COMMAND` rules (`icpx -fsycl`),
  so `write-compile-commands.py` — which exports only the native
  `c_COMPILER` / `cpp_COMPILER` rules — never saw them, and
  `make tidy-ratchet LANE=sycl` never ran
  `scripts/ci/gen-sycl-compile-commands.py`, which exists precisely
  to synthesise those entries. The committed
  `scripts/ci/tidy-baseline-sycl.json` therefore recorded an empty
  backend. The Makefile now runs the generator as a per-lane
  compile-database hook in both `tidy-ratchet` and
  `tidy-ratchet-write`, and documents the two build-dir
  preconditions the GPU lanes need: `-Db_lto=false` (ADR-1172's
  `b_lto_threads=4` renders as GCC's `-flto=4`, which clang rejects,
  failing every TU — the same workaround the CPU lane in
  `lint-and-format.yml` already applies) and an out-of-repo build
  directory, so meson's generated model sources stay out of the
  measurement. The cuda and sycl baselines are re-measured with
  `tidy-ratchet.py --write` (clang-tidy 22.1.8, **0 compile failures**
  on either lane), reported as TUs / warnings / uncited NOLINTs —
  cuda 351 / 1449 / 3 → **353 / 1418 / 0**, sycl 321 / 815 / 2 →
  **345 / 951 / 0**. The sycl lane's +24 TUs are the SYCL work it had
  never seen, so its warning rise is newly-visible pre-existing debt —
  now bounded by the ratchet — not a regression. The hip baseline keeps
  the warning counts this branch already measured and earned
  (343 / 1234), because the change is warning-neutral there: measuring
  `core/test/test_hip_float_vif_parity.c` before and after the citation
  edit returns the identical 19 warnings. Only its uncited-NOLINT
  metric moves, 1 → **0**, recomputed with the ratchet's own
  `scan_nolints()` — a pure text scan that needs no clang-tidy run.
  Every lane's `nolint_uncited` is now empty. No baseline was
  hand-edited and no allowance was widened.
