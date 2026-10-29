---
paths:
  - tools/vmaf-tune/src/vmaftune/bisect.py
  - tools/vmaf-tune/tests/test_bisect*.py
invariant: Bisect assumes monotone VMAF; default targets 94,96,97,98; PredicateFn signature is Phase B contract.
---
<!-- markdownlint-disable MD024 -->
# Target-VMAF bisection

- **`--target-vmafs` default sweep is `94,96,97,98` (ADR-0538,
  supersedes ADR-0534's `75,80,85,90,93`).** Premium-archival
  operating points; fork's primary user encodes archival masters at
  VMAF >= 95 exclusively. Back-compat for legacy scripts that pin
  single VMAF via `--target-vmaf NN` preserved by
  `_TrackedDefaultAction` sentinel: when `--target-vmaf` explicit
  and `--target-vmafs` at default, v1 single-target schema honoured.
  Sentinel detection happens in `_run_compare` — if you bypass
  `main()` and invoke `_run_compare` directly, stamp
  `args._target_vmafs_was_default` and
  `args._target_vmaf_was_default` first (call
  `_stamp_tracked_default_sentinels(args)`).
- **Bisect default search window is encoder's absolute CRF range
  (ADR-0538), not adapter's `quality_range`.** When
  `bisect_target_vmaf` is called with `crf_range=None` it consults
  registry `absolute_range` (ADR-3388, `api/vmafx-encoders.toml`) to pick encoder's
  accepted bounds: `libx264 / libx265 -> (0, 51)`,
  `libvpx-vp9 / libaom-av1 / libsvtav1 -> (0, 63)`. This is wider
  than perceptually-informative `adapter.quality_range` (e.g.
  libx265's `(15, 40)`) so high-VMAF targets are reachable. Bisect
  also bypasses `adapter.validate`'s CRF gate, re-implements only
  preset + absolute-range checks in `_encode_and_score`; if you
  change `adapter.validate` semantics also audit
  `bisect._encode_and_score` to keep them in sync. Corpus-generator
  path in `corpus.py` still calls `adapter.validate` unchanged —
  only bisect search loop widened. Adding new codec to
  absolute-range table is single dict entry; codecs not in table
  fall back to `adapter.crf_min/crf_max` then `quality_range`.
- **Phase B bisect assumes monotone-decreasing VMAF in CRF
  ([ADR-0326](../../../docs/adr/0326-vmaf-tune-phase-b-bisect.md)).**
  `vmaftune.bisect.bisect_target_vmaf` aborts with clear error when
  two non-adjacent samples violate this contract by more than 0.5
  VMAF (looser than measurement noise). Never weaken to fall-back
  search strategy on monotonicity violation — contract is part of
  public surface, and surfacing violation is more useful than
  papering over it. Real-world content + modern codecs satisfy
  contract; pathological exceptions are encoder bugs we want to
  see, not absorb. Subprocess seam mirrors `encode.run_encode` /
  `score.run_score`: tests inject `encode_runner` / `score_runner`
  stubs; production callers leave them `None`.
- **`bisect_target_vmaf` public kwarg `workdir`** — added by
  ADR-0598. Resolution order: `workdir=` kwarg (explicit Path) >
  `VMAFTUNE_WORKDIR` env var > OS default (`/tmp`). Private
  helpers `_workdir_parent`, `_estimate_yuv_bytes`, and
  `_check_disk_space` are **not** in `__all__` —
  `test_module_exports_match_public_surface` in
  `tests/test_bisect.py` pins exact set `{BisectResult,
  BisectSample, bisect_target_vmaf, make_bisect_predicate}`. Adding
  private helpers to `__all__` will trip that test; import them
  directly in tests if needed. `make_bisect_predicate` forwarding
  call in `_run_compare` (cli.py) includes `workdir=args.workdir`;
  any new compare-path caller must carry this kwarg through or
  `test_cli_compare_binds_real_bisect_predicate` assertion will
  catch omission.
  ([ADR-0598](../../../docs/adr/0598-vmaftune-workdir-relocation.md))

## Phase D rebase-sensitive invariants

- **Predicate signature is Phase B contract.** ``PredicateFn``
  type alias in ``per_shot.py`` is ``(Shot, target_vmaf: float,
  encoder: str) -> (crf: int, measured_or_predicted_vmaf: float)``.
  CLI adapter around Phase-B bisect must conform to this
  signature; widening return tuple is coordinated change that
  bumps public-API surface across both modules in same PR.
- **Bisect inputs are temporary raw YUV shots.**
  `bisect_target_vmaf` expects raw YUV geometry, so CLI extracts
  each detected half-open shot range to temporary raw-YUV file
  before calling it. Raw `.yuv` / `.raw` sources are opened with
  explicit rawvideo demuxer flags (`--width`, `--height`,
  `--pix-fmt`, `--framerate`); container and Y4M sources are left
  to FFmpeg's demuxer.
