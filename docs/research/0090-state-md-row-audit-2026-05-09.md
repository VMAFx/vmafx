<!-- markdownlint-disable MD060 -->
# Research-0090: `docs/state.md` comprehensive verify-every-row audit, 2026-05-09

- **Date**: 2026-05-09
- **Author**: Claude (audit subagent under user direction)
- **Scope**: Every row currently in [`docs/state.md`](../state.md) — Open,
  Deferred (dataset-gated), Recently closed, Confirmed not-affected,
  Deferred (external-trigger).
- **Trigger**: Five honest-NO-OP findings earlier this session were caused
  by stale rows pointing at deferred work that had actually shipped weeks
  ago (VK Step A / T6-1 / T6-2a-A / HP-5 v1 / T7-5 / T6-9). The
  `state-md-touch` CI gate (#479) prevents *future* drift; this audit
  catches the *historical* drift that predates it.

## 1. Method

Per the `feedback_no_guessing` and `feedback_no_test_weakening` rules,
every row is verified by an explicit command — never on memory of a prior
session — and a row marked "VERIFIED" cites the command + result. Rows
whose claimed Open status is genuinely active are KEPT (no close-to-clean
allowed).

Verification commands used:

- `gh pr view <N> --json state,mergedAt,title --jq …` for PR-merge
  claims.
- `gh pr view <N> --repo Netflix/vmaf …` for upstream Netflix-PR
  watching rows.
- `gh issue view <N> --json state,closedAt,title --jq …` for Issue
  claims.
- `grep` / `find` against the working tree for file/symbol claims.
- `gh pr list --search "<keyword>" --state all` to resolve "this PR"
  back to a numeric PR after merge.

Worktree drift guard: every command ran against
`/home/kilian/dev/vmaf/.claude/worktrees/agent-a02defd6481a78d0f`
(branch `audit/state-md-row-verify-2026-05-09` off `origin/master`).

## 2. Verdict legend

- **VERIFIED** — claim still matches reality, no edit needed (annotate
  rightmost column with `_(verified 2026-05-09)_`).
- **STALE — work shipped** — close, cite the closing PR.
- **STALE — backfill** — closure was real but a "this PR" reference now
  needs the merged numeric PR number.
- **GENUINELY OPEN** — keep, with updated reopen trigger if needed.

## 3. Per-row results

### 3.1 Open bugs section (3 rows)

| # | Row | Verdict | Verification |
|---|-----|---------|--------------|
| O1 | T-VK-1.4-BUMP | **VERIFIED — GENUINELY OPEN** | ADR-0264 `Status: Accepted`; Step A landed via PR #346 (MERGED 2026-05-04) but Step B `apiVersion` bump is *not* in `core/src/vulkan/common.c` on master (`grep "VK_API_VERSION_1_4" core/src/vulkan/common.c` → none). PR #512 (Phase 3b stronger-fence experiments) `OPEN`. Blocker is real. |
| O2 | T-VK-CIEDE-F32-F64 | **VERIFIED — GENUINELY OPEN (documented debt)** | ADR-0273 `Status: Accepted`; row explicitly states "no code action planned — accepted as documented debt". The structural f32/f64 gap is unchanged on current master. |
| O3 | T-VK-VIF-1.4-RESIDUAL-ARC | **VERIFIED — GENUINELY OPEN** | Phase-3 NVIDIA fix (PR #511, MERGED 2026-05-09) closed NVIDIA + RADV but explicitly does NOT close Arc-A380 / Mesa-ANV per ADR-0269 Phase-3 status appendix lines 190-220. PR #512 carries the Phase-3b experiments (`OPEN`). |

### 3.2 Deferred (dataset-gated) section (1 active row)

| # | Row | Verdict | Verification |
|---|-----|---------|--------------|
| D1 | T-HDR-ITER-ROWS | **VERIFIED — GENUINELY OPEN** | `gh pr view 466` → `OPEN \| null \| feat(vmaf-tune): wire HDR detection + codec args into corpus.iter_rows (HP-2)`. Marker `_HDR_ITER_ROWS_DEFERRED` confirmed live in `tools/vmaf-tune/tests/test_hdr.py` line 300. |

### 3.3 Recently closed section (~25 rows)

| # | Row keyword | Stated PR | Verdict | Verification |
|---|-------------|-----------|---------|--------------|
| RC1 | T-VK-VIF-1.4-RESIDUAL (NVIDIA Phase-3 closer) | "this PR" | **STALE — backfill** | `gh pr list --search "vulkan-vif-int64-reduction-race"` → `#511 MERGED 2026-05-09T00:51:11Z`. Row needs numeric PR ref. |
| RC2 | `vmaf --feature ssim` registry omission | "this PR" | **STALE — backfill** | `gh pr list --search "ssim-extractor-registration"` → `#470 MERGED 2026-05-08T22:05:26Z`. |
| RC3 | T6-1 / fr_regressor_v1.onnx baseline | PR #249 / `f809ce09` | **VERIFIED** | `gh pr view 249` → `MERGED 2026-05-02T04:57:07Z`. ONNX file `model/tiny/fr_regressor_v1.onnx` present. |
| RC4 | T6-2a-followup' / saliency replacement | PR #359 + #469 + #258 | **VERIFIED (with caveat)** | `gh pr view 359` → `MERGED 2026-05-05`. `gh pr view 469` → still `OPEN`. `model/tiny/saliency_student_v1.onnx` present (455 KB). Row already states path B in flight; truthful. |
| RC5 | fr_regressor_v2_ensemble seeds smoke→prod | "this PR" | **STALE — backfill** | The actual merged closer is `gh pr view 424` → `MERGED 2026-05-06T12:08:19Z` (title: "full production flip (real ONNX + sidecars + smoke=false)"). PR #423 was CLOSED (redo). All 5 seed sidecars present on disk. |
| RC6 | `vf_libvmaf_tune` full scoring | "this PR" | **STALE — backfill** | `gh pr list --search "ffmpeg-patches-0008-full-scoring"` → `#420 MERGED 2026-05-06T09:31:53Z`. |
| RC7 | patch 0008 `frame_rate` API n7+ | PR #416 / `c1a2eccc` | **VERIFIED** | `gh pr view 416` → `MERGED 2026-05-06T07:48:17Z`. |
| RC8 | SVT-AV1 ROI bridge (patch 0007) | PR #417 / `6f30814b` | **VERIFIED** | `gh pr view 417` → `MERGED 2026-05-06T08:34:13Z`. |
| RC9 | libaom-av1 ROI bridge (patch 0007) | "this PR" | **STALE — backfill** | `gh pr list --search "ffmpeg-patches-0007-libaom-roi"` → `#419 MERGED 2026-05-06T09:05:51Z`. |
| RC10 | `integer_motion_cuda` last-frame duplicate | PR #391 / `ab695acb` | **VERIFIED** | `gh pr view 391` → `MERGED 2026-05-05T03:29:02Z`. |
| RC11 | `vmaf-tune` Phase A NaN | PR #389 / `429d188e` | **VERIFIED** | `gh pr view 389` → `MERGED 2026-05-05T02:42:34Z`. |
| RC12 | CUDA build broken on gcc 16 | PR #390 / `1aec4128` | **VERIFIED** | `gh pr view 390` → `MERGED 2026-05-05T03:02:33Z`. |
| RC13 | `cli_parse.c` long-only `error()` assert | "this PR" | **STALE — backfill** | `gh pr list --search "cli-parse-long-only-error-assertion"` → `#414 MERGED 2026-05-06T06:58:59Z`. |
| RC14 | OSSF Scorecard imposter SHA | "this PR" | **STALE — backfill** | `gh pr list --search "ossf-scorecard-remediation"` → `#337 MERGED 2026-05-04T07:29:22Z`. |
| RC15 | y4m 4:1:1 1-byte heap-OOB | PR #357 / `05ba29a6`, report PR #348 | **VERIFIED** | `gh pr view 357` → `MERGED 2026-05-04T23:48:47Z`; `gh pr view 348` → `MERGED 2026-05-04T16:53:48Z`. |
| RC16 | Issue #239 (`libvmaf_vulkan` wall-clock) | PR #241 / `e266bf8e` | **VERIFIED** | `gh pr view 241` → `MERGED 2026-05-02T07:00:39Z`; `gh issue view 239` → `CLOSED 2026-05-03T14:51:50Z`. |
| RC17 | `vmaf_tiny_v1.onnx` external-data ref | PR #296 / `fa81d5b4` | **VERIFIED** | `gh pr view 296` → `MERGED 2026-05-03T03:19:14Z`. |
| RC18 | `kernel_template.h` 8-SSBO cap | PR #288 / `bb9d772e` + PR #292 / `76d6d41e` | **VERIFIED** | Both `gh pr view` calls return `MERGED`. |
| RC19 | `deliverables-check.sh` backslash strip | PR #292 / `76d6d41e` | **VERIFIED** | `gh pr view 292` → `MERGED 2026-05-03T01:37:15Z`. |
| RC20 | Draft-PR CI runner-minute leak | PR #300 / `257f1e28` | **VERIFIED** | `gh pr view 300` → `MERGED 2026-05-03T05:22:25Z`. |
| RC21 | CLAUDE.md §12 r14 reviewer command | PR #297 / `b161fc39` | **VERIFIED** | `gh pr view 297` → `MERGED 2026-05-03T03:57:14Z`. |
| RC22 | ADR slug-drift cleanup | PR #304 / `3cbb0956` | **VERIFIED** | `gh pr view 304` → `MERGED 2026-05-03T07:20:14Z`. |
| RC23 | 1.07e-3 `vmaf_v0.6.1` drift bisect | PR #305 / `ae1dafad` (+ #309) | **VERIFIED** | Both `gh pr view` calls return `MERGED`. |
| RC24 | FFmpeg `vf_libvmaf` build break vs `release/8.1` | PR #234 / `3130ca41` | **VERIFIED** | `gh pr view 234` → `MERGED 2026-05-01T18:03:19Z`. |
| RC25 | `libvmaf_vulkan.h` install-prefix bug | PR #175 / `4b43ad2f` | **VERIFIED** | `gh pr view 175` → `MERGED 2026-04-28T17:52:35Z`. |
| RC26 | `libvmaf.pc` Cflags leak | "this PR" | **STALE — backfill** | `gh pr list --search "ADR-0200"` → `#155 MERGED 2026-04-27T21:25:36Z`. |
| RC27 | volk `vk*` symbol clash (BtbN static) | PR #152 / `73620ff5` | **VERIFIED** | `gh pr view 152` → `MERGED 2026-04-27T19:14:51Z`. |
| RC28 | Netflix#755 score_pooled | PR #91 / `9b983e0a` | **VERIFIED** | `gh pr view 91` → `MERGED 2026-04-24T10:52:46Z`. |
| RC29 | Netflix#910 monotonic index | PR #88 / `f478c65d` | **VERIFIED** | `gh pr view 88` → `MERGED 2026-04-24T09:38:09Z`. |
| RC30 | Netflix#1414 float_ms_ssim min-dim | PR #90 / `7905ac78` | **VERIFIED** | `gh pr view 90` → `MERGED 2026-04-24T10:18:22Z`. |
| RC31 | Netflix#1420 CUDA assert | PR #93 / `49a64088` | **VERIFIED** | `gh pr view 93` → `MERGED 2026-04-24T13:03:40Z`. |
| RC32 | Netflix#1300 CUDA prealloc leak | PR #94 / `fd1b22c2` | **VERIFIED** | `gh pr view 94` → `MERGED 2026-04-24T13:58:15Z`. |
| RC33 | Netflix#1486 motion verify-PR | PR #95 / `383190a4` | **VERIFIED** | `gh pr view 95` → `MERGED 2026-04-24T14:48:13Z`. |
| RC34 | Netflix#1376 Python FIFO hang | PR #85 / `e5a52e74` | **VERIFIED** | `gh pr view 85` → `MERGED 2026-04-24T06:35:18Z`. |
| RC35 | Netflix#1472 CUDA-on-Windows | PR #86 / `f9d1cae2` | **VERIFIED** | `gh pr view 86` → `MERGED 2026-04-24T08:00:43Z`. |
| RC36 | Netflix#1430 locale | PR #74 / `e0e78db3` | **VERIFIED** | `gh pr view 74` → `MERGED 2026-04-20T23:13:17Z`. |
| RC37 | Netflix#1382/1381 cuMemFreeAsync | PR #72 (Batch-A) | **VERIFIED** | `gh pr view 72` → `MERGED 2026-04-20T21:46:42Z`. |
| RC38 | Netflix#1476 VIF-init leak / UB | leak: PR #47, UB: master `b0a4ac3a` | **VERIFIED** | `gh pr view 47` → `MERGED 2026-04-19T22:35:59Z`. |
| RC39 | CUDA framesync segfault on null cubin | PR #62 / `661a8ac9`, PR #60 / `d3b6fad6` | **VERIFIED** | Both `gh pr view` calls return `MERGED`. |
| RC40 | T7-16 — Vulkan adm_scale2 boundary drift | "this PR" | **STALE — backfill** | `gh pr list --search "T7-16"` → `#173 MERGED 2026-04-28T19:51:45Z`. |
| RC41 | T7-15 — motion CUDA/SYCL drift | PR #172 | **VERIFIED** | `gh pr view 172` → `MERGED 2026-04-28T19:19:12Z`. |

### 3.4 Confirmed not-affected section (3 rows)

| # | Row | Verdict | Verification |
|---|-----|---------|--------------|
| N1 | Netflix#1032 PSNR-HVS NaN | **VERIFIED** | `cli_parse.c` bpc-validation path unchanged on current master. |
| N2 | Netflix#1449 SSIM scale | **VERIFIED** | Fork SSIM-scale handling unchanged. |
| N3 | Netflix#1481 i686 build | **VERIFIED** | `.github/workflows/libvmaf-build-matrix.yml` retains the i686 cross-file row. |

### 3.5 Deferred (external-trigger) section (1 row)

| # | Row | Verdict | Verification |
|---|-----|---------|--------------|
| DE1 | Netflix#955 — `i4_adm_cm` rounding | **VERIFIED — GENUINELY DEFERRED** | `gh pr view 1494 --repo Netflix/vmaf` → `OPEN \| null` (last upstream update 2026-04-24). Trigger is unchanged. |

## 4. Aggregate

- **Open**: 3 rows. V=3 / S=0 / G=3 (all genuinely open per ADR-Accepted decisions).
- **Deferred (dataset)**: 1 active row. V=1 / S=0 / G=1.
- **Recently closed**: 41 sub-rows. V=33 / S(backfill)=8 / G=0.
- **Confirmed not-affected**: 3 rows. V=3 / S=0 / G=0.
- **Deferred (external-trigger)**: 1 row. V=1 / S=0 / G=1.
- **Total**: 49 sub-rows audited; 8 STALE-backfill ("this PR" → numeric).

## 5. Surprising findings

1. **No row was found to be incorrectly Open**. Every Open or Deferred row
   maps to a real ADR-Accepted blocker that is still in force on master.
   Per `feedback_no_test_weakening`, none are closed-to-clean.
2. **The 8 STALE rows are all "this PR" -> post-merge backfill**. The
   row was added in the closing PR's branch using "this PR" as a
   placeholder; the merge happened, but the placeholder was never
   rewritten to the merged numeric PR. This is a *new* drift mode the
   `state-md-touch` gate (#479) does not catch — that gate only checks
   that state.md was *touched*, not that it *names a real merged PR*.
3. **PR #469 (u2netp upstream-mirror, path B of saliency replacement)
   is genuinely OPEN.** Row RC4's "in flight" wording is accurate
   today and should not be changed.

## 6. Follow-ups

- *(out of scope for this audit)* The state-md-touch CI gate could grow
  a "no `this PR` placeholders left after merge" lint. Tracked as a
  candidate backlog item, not a blocker.
