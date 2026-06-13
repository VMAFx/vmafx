<!-- markdownlint-disable MD060 -->
# ADR-0262: bisect-model-quality cache check uses logical comparison for parquet

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, ci, tiny-ai, framework
- **Supersedes**: relaxes the byte-equality clause from
  [ADR-0109](0109-nightly-bisect-model-quality.md) §Decision (parquet
  artefacts only); ONNX byte-equality is preserved.

## Context

Issue [#40](https://github.com/VMAFx/vmafx/issues/40) is the sticky
tracker for the nightly `bisect-model-quality` workflow defined in
[ADR-0109](0109-nightly-bisect-model-quality.md). The original ADR
asserted full byte-equality between the committed
`ai/testdata/bisect/features.parquet` and the regenerator's output, and
flagged "rebases that change `pandas` or `pyarrow` versions can break
the drift check; the fix is regenerate + commit" under §Negative.

That maintenance cost has now landed, harder than expected. From
2026-04-22 through 2026-05-03 every nightly run has failed at the
`build_bisect_cache.py --check` step with `DRIFT byte drift:
features.parquet`. The cause is the runner image's pyarrow upgrading
from 23.x to 24.x: the parquet writer embeds `created_by:
parquet-cpp-arrow version <X>.<Y>.<Z>` in the file header, so byte
identity flips on every minor pyarrow release the
`actions/setup-python` toolcache adopts. Worse, the workflow's
sticky-comment update is gated on the result.json artefact existing
(`if: hashFiles('bisect-out/result.json') != ''`), and the `--check`
failure produces no result.json — so the comment on issue #40 stayed
frozen on the 2026-04-21 green verdict for ~14 days while the workflow
silently red-lined every night.

The committed parquet's actual content (256 rows × 7 typed columns,
fixed-seed values) is unchanged; only the writer-version string
drifted. Byte-equality on parquet is the wrong gate for the drift
guarantee the ADR-0109 §Decision was after — "catching drift in pandas
/ pyarrow / onnx serialisation" should mean "catch a content change",
not "catch a version-string change".

## Decision

Relax the parquet leg of the cache `--check` to a typed-Arrow-Table
comparison; keep ONNX byte-equality as-is. Concretely:

- `ai/scripts/build_bisect_cache.py` adds `_compare_parquet`, which
  reads both files via `pyarrow.parquet.read_table` and compares
  schema, row count, and `Table.equals` content. The parquet header's
  `created_by` field is no longer load-bearing.
- `_compare_onnx` keeps `filecmp.cmp(shallow=False)`. ONNX
  determinism is held by the existing pinning of `producer_name`,
  `producer_version`, and `ir_version` in `_save_linear_fr`; weight,
  opset, or graph-topology drift continues to trip immediately.
- The unknown-extension fallback in `check()` still uses byte
  comparison so a future artefact format does not slip through
  unguarded.
- `nightly-bisect.yml` decouples the cache-check step's fail-fast
  semantics from the sticky-comment update: when `--check` fails,
  `post-bisect-comment.py --wiring-broke --error-log <stderr>` posts
  a "WIRING BROKE" verdict to issue #40 with the cache-check stderr
  inline, then the workflow exits non-zero so the run is still red.
  The result.json path stays the success path; the wiring-broke path
  is the failure path; both lead to a fresh sticky comment.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Logical (Arrow Table) parquet compare + onnx byte compare** (chosen) | Tolerates harmless writer-version drift; still catches content/schema/row-count drift; one-time fix; no recurring maintenance | Loses byte-level detection of compression-codec or footer-layout changes (none of which has ever produced a real signal) | Best fit: the failure mode we keep hitting is exactly the one this skips |
| Pin `pyarrow==23.0.1` in `ai/pyproject.toml` and the workflow | Restores byte equality immediately; no script change | Brittle: every `dependabot` / `renovate` bump or transitive constraint forces another regeneration cycle; conflicts with `pyarrow>=17.0` floor that real training jobs need | Punts the problem; doesn't solve "version-string-as-load-bearing-byte" |
| Regenerate the committed cache against pyarrow 24, commit, repeat per ADR-0109 §Negative | Lowest blast radius; matches the ADR's documented escape valve | Treadmill: the next pyarrow release breaks it again; consumed 14 days of stale comment already with nobody noticing | Doesn't scale; the maintenance cost the ADR accepted turned out to be unaffordable |
| Strip `created_by` from the committed parquet via post-write rewrite | Keeps byte-equality semantics for the rest of the file | Requires a custom parquet rewriter; non-portable across pyarrow versions; the parquet spec doesn't promise stable footer layout for the same content | Fragile, undertested |
| Move the cache check into the bisect tool itself and skip on drift | Simpler workflow | Hides drift entirely; defeats the purpose of the gate | Worse than status quo |
| Switch the cache to a CSV / JSON encoding for trivial byte stability | Easy byte equality | Loses typed dtypes the bisect consumes; doubles fixture size; misses the point that real DMOS caches will be parquet | Wrong direction for the eventual real-cache swap |

## Consequences

- **Positive**:
  - Issue #40 unsticks — nightly verdict reflects the previous night's
    actual run again, including a "WIRING BROKE" path for future
    `--check` failures so the comment never silently freezes.
  - Toolchain bumps that touch only writer metadata (pyarrow minor /
    patch releases, `--compression` choice changes, footer-layout
    rewrites) no longer red-line the nightly.
  - The drift guarantee that matters — "row values, schema, model
    weights, opset, or ir_version changed silently" — is preserved
    via Arrow-Table equality + ONNX byte equality.
- **Negative**:
  - A pyarrow release that actually corrupts row values (vs writes a
    bit-identical-but-byte-different file) is caught by content
    equality, not by header drift, so the failure point shifts one
    layer deeper — but that is the correct layer.
  - Real future content drift now requires the maintainer to read the
    `_compare_parquet` diff message ("schema drift" / "row-count
    drift" / "row-content drift") rather than a generic "byte drift"
    line. The diagnostic surface improves; the muscle memory shifts.
- **Neutral / follow-ups**:
  - Real DMOS-aligned cache swap from
    [Research-0001](../research/0001-bisect-model-quality-cache.md)
    inherits the same `_compare_parquet` shape; no further check-script
    work expected at swap time.
  - If a future artefact ships in a third format (e.g. `.npz`), the
    fallback byte-compare branch continues to apply until that format
    grows its own `_compare_<ext>` helper.

## References

- Issue [#40](https://github.com/VMAFx/vmafx/issues/40) — sticky
  bisect tracker; the comment that froze on 2026-04-21.
- [ADR-0109](0109-nightly-bisect-model-quality.md) — parent decision
  on the nightly bisect workflow + synthetic placeholder cache.
- [Research-0001](../research/0001-bisect-model-quality-cache.md) —
  cache shape alternatives and the eventual real-cache swap path.
- [`ai/scripts/build_bisect_cache.py`](../../ai/scripts/build_bisect_cache.py)
  — the regenerator and `--check` driver.
- [`scripts/ci/post-bisect-comment.py`](../../scripts/ci/post-bisect-comment.py)
  — the sticky-comment writer; gains a `--wiring-broke` mode.
- [`.github/workflows/nightly-bisect.yml`](../../.github/workflows/nightly-bisect.yml)
  — the workflow whose silent failure motivated the change.
- Source: per user direction in this issue's diagnostic task —
  "diagnose why the workflow stopped firing and post either a fix or
  a manual run". Fix preferred over manual rerun because the underlying
  cause was structural, not transient.

## Reproducer / smoke-test

```bash
# Logical-equality path: still passes against the committed cache.
python ai/scripts/build_bisect_cache.py --check

# Force a known-good byte-different rewrite to confirm the parquet
# leg now tolerates it (compression codec swap forces footer drift):
python -c "
import pyarrow.parquet as pq, tempfile, filecmp
from pathlib import Path
import sys; sys.path.insert(0, 'ai/scripts')
import importlib.util
spec = importlib.util.spec_from_file_location('m', 'ai/scripts/build_bisect_cache.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
src = Path('ai/testdata/bisect/features.parquet')
with tempfile.TemporaryDirectory() as t:
    dst = Path(t) / 'features.parquet'
    pq.write_table(pq.read_table(str(src)), str(dst), compression='snappy')
    assert not filecmp.cmp(str(src), str(dst), shallow=False)
    assert m._compare_parquet(src, dst) is None
    print('OK: byte-different, logically equal')
"

# Force a content drift to confirm the gate still catches it:
python -c "
import pyarrow as pa, pyarrow.parquet as pq, tempfile
from pathlib import Path
import sys; sys.path.insert(0, 'ai/scripts')
import importlib.util
spec = importlib.util.spec_from_file_location('m', 'ai/scripts/build_bisect_cache.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
src = Path('ai/testdata/bisect/features.parquet')
with tempfile.TemporaryDirectory() as t:
    dst = Path(t) / 'features.parquet'
    df = pq.read_table(str(src)).to_pandas()
    df.iloc[0, 0] = 99.0
    pq.write_table(pa.Table.from_pandas(df, preserve_index=True), str(dst))
    assert m._compare_parquet(src, dst) == 'row-content drift: features.parquet'
    print('OK: content drift detected')
"
```
