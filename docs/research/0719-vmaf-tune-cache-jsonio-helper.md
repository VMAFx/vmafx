<!-- markdownlint-disable MD060 -->
# Research-0719 — vmaf-tune cache strict JSON metadata

## Question

Can the new `vmaftune.jsonio.write_json_strict()` helper cover the tune cache
without changing cache-key identity or corpus JSONL missing-feature semantics?

## Findings

- `cache_key()` must keep its compact canonical JSON digest unchanged. Any
  formatting or non-finite rewrite there would change cache identity and violate
  ADR-0298.
- `TuneCache._write_index()` and `TuneCache.put()` metadata writes are
  sidecars, not interchange corpus rows. They can use strict JSON safely.
- A non-finite cached `vmaf_score` is not useful. Replaying it on a hit hides
  the bad score behind the cache, so the safer behavior is to make the entry a
  miss and force a fresh encode/score attempt.

## Decision

Route cache index and metadata JSON sidecars through `write_json_strict()`, keep
`cache_key()` on the existing compact digest path, and treat strict-nullified
metadata values as a miss during `get()`.

## Alternatives considered

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Convert all cache JSON uses | Uniform helper use | Changes cache-key digest construction risk | Rejected |
| Leave metadata on raw `json.dump()` | Smallest diff | Cache sidecars can contain non-standard JSON tokens | Rejected |
| Strict sidecars, digest unchanged, null score as miss | Portable metadata and stable keys | Bad entries are recomputed on next access | Chosen |

## Validation

```bash
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_cache.py -q
.venv/bin/ruff check tools/vmaf-tune/src/vmaftune/cache.py tools/vmaf-tune/tests/test_cache.py
.venv/bin/black --check tools/vmaf-tune/src/vmaftune/cache.py tools/vmaf-tune/tests/test_cache.py
```
