<!-- markdownlint-disable MD013 MD060 -->
# ADR-0597: `integer_vif` is luma-only across every backend; CUDA `enable_chroma` is a documented no-op

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude Opus 4.7 (1M context)
- **Tags**: cuda, vif, parity, docs, audit-disposition

## Context

The 2026-05-18 deep audit (`.workingdir/bbb_reports/DEEP_AUDIT_2026_05_18.md`,
finding 23) flagged
`core/src/feature/cuda/integer_vif_cuda.c:180` (`s->n_planes = 1; /* CUDA
path: chroma dispatch not yet implemented */`) as a real "not implemented"
gap, on the premise that the CPU twin processes three planes and the CUDA
twin only one — and that this would be a source of cross-backend chroma
drift.

The premise is wrong on every count:

1. **CPU `integer_vif`** (`core/src/feature/integer_vif.c`, master)
   processes the luma plane only. Its `extract()` reads
   `ref_pic->data[0]` and `dist_pic->data[0]` directly (lines 804–815);
   there is no `enable_chroma` option in its `options[]` table and no
   loop over planes. The `init` callback discards `pix_fmt` with
   `(void)pix_fmt;` (line 624).
2. **CUDA `integer_vif_cuda`** mirrors that: its `submit_fex_cuda()` loop
   iterates `for (plane = 0; plane < s->n_planes; ++plane)` but
   `s->n_planes` is hardcoded to `1`, the loop body unconditionally
   reads `ref_pic->data[0]` / `dist_pic->data[0]`, and the underlying
   `filter1d_8` / `filter1d_16` PTX kernels only have a luma code path.
3. **Every other backend** (`x86/vif_avx2.c`, `x86/vif_avx512.c`,
   `arm64/vif_neon.c`, `hip/integer_vif_hip.c`,
   `sycl/integer_vif_sycl.cpp`, `vulkan/integer_vif_vulkan.c`,
   `metal/integer_vif_metal.mm`) reads `data[0]` only — confirmed by
   grep across the tree.
4. **Upstream Netflix/vmaf `master`**
   (`core/src/feature/cuda/integer_vif_cuda.c` at upstream HEAD
   `32780bd9b6`) hardcodes the same `data[0]`-only access and has no
   `n_planes` field at all.

VIF (Sheikh & Bovik, 2006) is *defined* on a single luminance channel.
The classic VMAF feature set has always been luma-only. Three sources
together seeded the false-positive audit reading:

- A 2026-05-16 feature branch (`feat/cpu-vif-enable-chroma`, PR #948,
  commit `5555fee5a` — never landed on `master`) attempted to add a
  CPU-side `enable_chroma` option mirroring the PSNR/SSIM pattern.
  That work was abandoned. Branch lineage on `master` does not
  contain the commit.
- The CUDA twin carries a vestigial `enable_chroma` option (added by
  PR #949 — commit `6c0cd771e` — also never on the `master` lineage
  this branch lands against). The field is parsed into the state but
  never read by the dispatch loop; the kernel has no chroma path.
  Setting `enable_chroma=true` silently produces luma-only output —
  the same output as the default.
- `docs/metrics/vif.md` advertises `integer_vif_cb` / `integer_vif_cr`
  features and an `enable_chroma=true` invocation example that *do
  not exist* on master.

The combination — a vestigial option + docs lying about features the
code never produces + a code comment reading "not yet implemented" —
made the audit reasonably suspect a real gap. There is no gap; there
is documentation drift.

## Decision

1. **Confirm luma-only as the long-term design** for `integer_vif`
   across every backend. VIF is, per definition, a single-channel
   metric. There is no plan to add multi-plane VIF; doing so would
   diverge from upstream and require a fresh research justification
   (per-channel VIF has no established MOS-correlation literature).
2. **Clarify, do not remove, the CUDA `enable_chroma` option.**
   Removing it would silently break any caller that already passes
   `integer_vif:enable_chroma=…` on the command line or in a model
   JSON. Instead:
   - Update the option help text to say it is a documented no-op
     retained for backward compatibility (the CUDA kernel is
     luma-only).
   - Emit a one-shot `vmaf_log VMAF_LOG_LEVEL_WARNING` when the
     option is set to `true`, telling the caller the request will
     be honoured as luma-only and pointing at this ADR.
   - Rewrite the misleading "not yet implemented" comment to
     "luma-only by design; matches CPU integer_vif".
3. **Correct `docs/metrics/vif.md`.** Remove the `integer_vif_cb` /
   `integer_vif_cr` rows from the output-features table and the
   `enable_chroma=true` invocation example. State explicitly that
   the metric is luma-only across every backend. Document the
   `enable_chroma` option's no-op nature and warning behaviour so
   anyone reading the docs in isolation gets the same story as
   anyone reading the code.
4. **Record the audit finding as Confirmed not-affected** in
   `docs/state.md`, citing this ADR and the cross-backend numerical
   parity test added by this PR.
5. **Add a CPU-vs-CUDA parity smoke test**
   (`core/test/test_integer_vif_cpu_cuda_parity.c`, suite `fast`,
   guarded by `enable_cuda`) running both backends against the
   Netflix `src01_hrc00_576x324.yuv` / `src01_hrc01_576x324.yuv`
   4:2:0 pair and asserting that the four `vif_scaleN_score`
   features agree within 1e-5. The test fixes the parity claim in
   regression terms so the audit story cannot drift back.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Status quo (no change)** | Smallest diff. | Leaves docs lying about non-existent chroma features and the misleading "not yet implemented" comment; the same audit class will refile next month. | Doc-substance rule (CLAUDE.md global rule 2) requires user-discoverable surfaces to match docs. |
| **Implement multi-plane VIF for parity with PSNR/SSIM** | Removes the vestigial option's deadness; mirrors the broader PSNR/SSIM/ANSNR `enable_chroma` family. | VIF has no MOS-correlation literature for chroma planes; would diverge from upstream Netflix; would require a new research digest, golden-data regeneration, and a multi-week investigation; the abandoned PR #948 attempt is evidence of the cost. | Out of scope for an audit-disposition PR; would need a fresh ADR + research digest. |
| **Remove the CUDA `enable_chroma` option entirely** | Cleanest internal model — option matches code behaviour. | Silently breaks any existing user invocation that sets the option (CLI flag, model JSON `feature_dict` entry). Even though the option was a no-op, removing it changes the option-not-recognised error path. | User-facing breaking change with no upside; the warn-on-set path conveys the same information without the breakage. |
| **Keep the option, leave the docs lying** | No risk of confusing users who relied on the (false) doc example. | The doc lies are the *bigger* user-confusion source than the silent no-op; anyone running the documented `enable_chroma=true` example sees the same scores as the default and quietly loses trust. | Documentation accuracy is the project's global rule 2 — non-negotiable. |

## Consequences

- **Positive**: the audit finding is closed with evidence (cross-backend
  parity test) rather than handwave. Docs now match the code. Future
  audits will not refile the same false-positive. Code comments stop
  misleading readers. CUDA users who set `enable_chroma=true` get a
  visible warning rather than silent luma-only behaviour.
- **Negative**: callers that set `enable_chroma=true` will see one
  warning per stream on stderr — surfacing previously-silent behaviour
  is the goal, but it is technically a log-output change.
- **Neutral / follow-ups**: if Netflix ever ships an upstream
  multi-plane VIF, the warning text in `integer_vif_cuda.c` and the
  docs in `vif.md` will both need updating. The vestigial option will
  flip from "no-op" to "active". The state.md row should then move
  from "Confirmed not-affected" to a normal closed-bug row at that
  point.

## References

- `req`: "Fix the integer_vif_cuda chroma-plane bug in VMAFx/vmafx
  surfaced by the deep audit (Finding 23 in
  `.workingdir/bbb_reports/DEEP_AUDIT_2026_05_18.md`) — paraphrased:
  if CPU also skips U/V (intentionally — VIF is luma-only by design),
  then the bug is the OPPOSITE: CPU also needs to match, OR the audit
  finding is a false-positive and we should document why n_planes=1
  is correct."
- Finding 23 in `DEEP_AUDIT_2026_05_18.md` (gitignored — local audit
  dossier under `.workingdir/bbb_reports/`).
- Upstream Netflix/vmaf `integer_vif_cuda.c` (commit `32780bd9b6`,
  fetched 2026-05-18) — confirms `data[0]`-only access in the CUDA
  VIF kernel; no `n_planes` field exists upstream.
- [ADR-0100](0100-project-wide-doc-substance-rule.md) — per-surface
  doc bar that the previous `vif.md` was violating.
- [ADR-0165](0165-state-md-bug-tracking.md) — state.md update rule
  triggered by this PR (Confirmed-not-affected row).
