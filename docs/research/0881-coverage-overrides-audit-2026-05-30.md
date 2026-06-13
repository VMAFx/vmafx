# Coverage-overrides audit — 2026-05-30

**Author**: Claude (Anthropic) for Lusoris
**Status**: Final
**Scope**: `scripts/ci/coverage-check.sh` `PER_FILE_MIN` map; cross-reference
each entry against fresh measured coverage; identify files needing overrides.
**Companion ADR**: [ADR-0881](../adr/0881-coverage-overrides-audit-2026-05-30.md).
**Master tip when audited**: `387839eacf`.

## TL;DR

| File | Override (pre-audit) | Actual | Action |
| ------ | ---------------------- | -------- | -------- |
| `core/src/dnn/ort_backend.c` | 78 % | 77.8 % | **Keep at 78** (PR #338 in flight raises actual to 78.5 % by adding `vmaf_ort_output_name_at` test) |
| `core/src/dnn/dnn_api.c` | 78 % | 78.0 % | **Keep at 78** (at structural ceiling per ADR-0114) |
| `core/src/dnn/tiny_extractor_template.h` | **10 %** | **77.4 %** | **Tighten to 75** (67.4 pp slack — silently giving up regression-lock) |

No new override entries required. All other `core/src/dnn/*` files plus
`opt.c` and `read_json_model.c` clear the global 85 % critical floor.

## Methodology

### Build flags (matches CI workflow `tests-and-quality-gates.yml`)

```bash
PKG_CONFIG_PATH=/home/kilian/.local/onnxruntime/lib/pkgconfig \
  meson setup core/build-coverage core --buildtype=debug \
    -Db_coverage=true -Denable_cuda=false -Denable_sycl=false \
    -Denable_float=true -Denable_avx512=true -Denable_dnn=enabled \
    -Dc_args=-fprofile-update=atomic -Dcpp_args=-fprofile-update=atomic
ninja -C core/build-coverage
LD_LIBRARY_PATH=/home/kilian/.local/onnxruntime/lib \
  meson test -C core/build-coverage --print-errorlogs --num-processes 1
```

Result: 63/63 unit tests pass.

### Coverage report

```bash
cd core && gcovr --root .. --filter 'src/.*' \
  --exclude '.*/test/.*' --exclude '.*/tests/.*' \
  --exclude '.*/subprojects/.*' \
  --gcov-ignore-parse-errors=negative_hits.warn \
  --gcov-ignore-parse-errors=suspicious_hits.warn \
  --print-summary \
  --txt build-coverage/coverage.txt \
  --json-summary build-coverage/coverage.json \
  --xml build-coverage/coverage.xml \
  build-coverage
```

Overall: **41.3 % line coverage** (above the 37 % floor enforced by
`coverage-check.sh`).

Note: the Python harness suite (`pytest python/test/`) was attempted
locally but hit libsvm timeouts on this host (CLAUDE.md §15 host-build
debt). The Python suite primarily contributes coverage to `opt.c` and
`read_json_model.c`; the meson unit suite alone is the load-bearing
contributor to `dnn/*.c` coverage. Both files already clear the global
85 % floor from the unit suite alone (100 % and 88 % respectively),
so missing Python coverage is not a confounding factor for this audit.

### Gate output

```text
Overall line coverage: 41.3000% (min 37%)
  critical: core/src/dnn/dnn_api.c — 78.0000% (min 78%)
  critical: core/src/dnn/dnn_attach_api.c — 92.0000% (min 85%)
  critical: core/src/dnn/model_loader.c — 86.9000% (min 85%)
  critical: core/src/dnn/onnx_scan.c — 93.4000% (min 85%)
  critical: core/src/dnn/op_allowlist.c — 100.0000% (min 85%)
  critical: core/src/dnn/ort_backend.c — 77.8000% (min 78%)
    FAIL: security-critical file below 78%        ← PR #338 fixes this
  critical: core/src/dnn/tensor_io.c — 98.1000% (min 85%)
  critical: core/src/dnn/tiny_extractor_template.h — 77.4000% (min 10%)
  critical: core/src/opt.c — 100.0000% (min 85%)
  critical: core/src/read_json_model.c — 88.0000% (min 85%)
```

`ort_backend.c`'s 77.8 % / 78 % fail is exactly the trip PR #338 fixes
(409 → 413 / 526 = 78.5 %); not this audit's scope.

## Per-override decision matrix

### 1. `core/src/dnn/ort_backend.c` — 78 % → 78 % (keep)

| Signal | Reading |
| -------- | --------- |
| Actual coverage | 77.8 % (526 lines, 410 covered) |
| Slack vs override | -0.2 pp (at cap, currently failing) |
| Root cause of stuck-at-cap | EP-attach success arms unreachable without OpenVINO/ROCm ORT build (ADR-0113); CreateSession→CPU fallback's nested error paths unreachable in healthy CI (ADR-0114) |
| In-flight fix | PR #338 — adds `vmaf_ort_output_name_at` unit test on a previously-untested production accessor. Lifts 410 → 413 covered → 78.5 % |
| **Action** | **Keep at 78** — PR #338 raises actual without raising bar (correct pattern per ADR-0114). Tightening to 78.5 % post-#338 = 0.0 pp slack, would flap. |

### 2. `core/src/dnn/dnn_api.c` — 78 % → 78 % (keep)

| Signal | Reading |
| -------- | --------- |
| Actual coverage | 78.0 % (173 lines, 135 covered) |
| Slack vs override | 0.0 pp (at cap) |
| Root cause of stuck-at-cap | EP-attach error categories the CPU EP can't trigger (ADR-0114 §Context); dead `has_norm` branch (`dnn_api.c:141-144`) |
| Available follow-up | Deleting the dead `has_norm` branch would lift this ~3 pp to ~81 % (ADR-0114 §Alternatives item 2). Independent cleanup; not this PR. |
| **Action** | **Keep at 78** — structural ceiling rationale unchanged. |

### 3. `core/src/dnn/tiny_extractor_template.h` — 10 % → 75 % (tighten)

| Signal | Reading |
| -------- | --------- |
| Actual coverage | 77.4 % (124 lines, 96 covered) |
| Slack vs override | **67.4 pp** |
| Root cause of slack | Original 10 % was set when the refactor first landed with one consumer (`feature_lpips.c`). Four extractors now instantiate the helpers (`feature_lpips.c`, `fastdvdnet_pre.c`, `feature_mobilesal.c`, `feature_transnet_v2.c`); cumulative consumption far exceeds the original worst-case projection. |
| New threshold | **75** — 2.4 pp slack matching the 1.3-1.7 pp slack ADR-0114 used for at-cap entries. Locks the gain; below 75 % means real regression. |
| **Action** | **Tighten 10 → 75** |

## New files surveyed for first-time override candidacy

Every file matching `*core/src/dnn/*`, plus `opt.c` and `read_json_model.c`
(the literal cases in `coverage-check.sh`'s critical-file glob):

| File | Coverage | Above global 85 %? | Action |
| ------ | ---------- | -------------------- | -------- |
| `core/src/dnn/dnn_attach_api.c` | 92.0 % (50 lines) | ✓ | None |
| `core/src/dnn/model_loader.c` | 86.9 % (659 lines) | ✓ | None |
| `core/src/dnn/onnx_scan.c` | 93.4 % (197 lines) | ✓ | None |
| `core/src/dnn/op_allowlist.c` | 100.0 % (7 lines) | ✓ | None |
| `core/src/dnn/tensor_io.c` | 98.1 % (270 lines) | ✓ | None |
| `core/src/opt.c` | 100.0 % (69 lines) | ✓ | None |
| `core/src/read_json_model.c` | 88.0 % (449 lines) | ✓ | None |

No new override entries required. `model_loader.c` at 86.9 % is closest
to the 85 % floor; worth keeping an eye on, but not over the line.

## Codified audit rule (lands in ADR-0881)

1. **Tighten when slack > 5 pp.** Override is silently giving up
   regression-lock; ratchet to `actual − 2 pp`.
2. **Keep at-cap entries.** Slack ≤ 2 pp = original ceiling rationale
   holds; leave alone.
3. **Remove when actual ≥ global 85 % floor.** Override redundant;
   delete and let the global gate enforce.
4. **Audit cadence: quarterly minimum, plus before any PR that
   modifies `PER_FILE_MIN`.**

## Verification of the change

After tightening `tiny_extractor_template.h` to 75 in
`scripts/ci/coverage-check.sh`:

```text
$ bash scripts/ci/coverage-check.sh core/build-coverage/coverage.json 37 85
Overall line coverage: 41.3000% (min 37%)
  critical: core/src/dnn/dnn_api.c — 78.0000% (min 78%)
  critical: core/src/dnn/dnn_attach_api.c — 92.0000% (min 85%)
  critical: core/src/dnn/model_loader.c — 86.9000% (min 85%)
  critical: core/src/dnn/onnx_scan.c — 93.4000% (min 85%)
  critical: core/src/dnn/op_allowlist.c — 100.0000% (min 85%)
  critical: core/src/dnn/ort_backend.c — 77.8000% (min 78%)
    FAIL: security-critical file below 78%        ← PR #338 territory, NOT this audit's concern
  critical: core/src/dnn/tensor_io.c — 98.1000% (min 85%)
  critical: core/src/dnn/tiny_extractor_template.h — 77.4000% (min 75%)   ← new threshold applied
  critical: core/src/opt.c — 100.0000% (min 85%)
  critical: core/src/read_json_model.c — 88.0000% (min 85%)
```

`tiny_extractor_template.h` now uses the 75 % threshold and clears it
with 2.4 pp slack. No other entries change behaviour.

## References

- [ADR-0114](../adr/0114-coverage-gate-per-file-overrides.md) — original
  per-file override design.
- [ADR-0881](../adr/0881-coverage-overrides-audit-2026-05-30.md) —
  companion decision recording the tightening + audit rule.
- [ADR-0110](../adr/0110-coverage-gate-fprofile-update-atomic.md) —
  atomic profile updates that make these per-file numbers honest.
- [ADR-0111](../adr/0111-coverage-gate-gcovr-with-ort.md) — gcovr (vs
  lcov) migration; ensures per-file numbers are deduped.
- PR #338 (`fix/coverage-gate-bbcaa8d127-v2`) — in-flight fix that
  pushes `ort_backend.c` from 77.8 % → 78.5 %.
- Source: `req` (paraphrased) user direction to audit per-file
  overrides for stale entries that have been improved + identify new
  files that should have overrides, "preserve 'never lower a threshold
  to bypass' — only tighten or add tests."
