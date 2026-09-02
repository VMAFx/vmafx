<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1097: Atomic file writes for AI-script cache and output files

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris
- **Tags**: `ai`, `correctness`, `reliability`

## Context

Several AI pipeline scripts write intermediate cache files (per-clip JSON) and
final output files (Parquet, JSONL) using non-atomic `Path.write_text()` or
`df.to_parquet()` calls. If the process is interrupted mid-write (OOM kill,
SIGKILL, power loss, disk full) the destination file is left partially
truncated. On the next resume attempt the file exists with a non-zero size, so
the resume logic treats it as a valid cached result and either crashes on a
`json.loads` parse error or silently produces corrupt output rows.

The specific patterns observed:

- `chug_extract_features.py` — `cache_path.write_text(...)` for the per-pair
  feature JSON (line 653) and `visual_cache_path.write_text(...)` for the
  visual-signal JSON (line 669). Both files are tested for existence at line
  611 before skipping re-extraction; a partial write permanently poisons the
  cache entry.
- `bvi_dvc_to_full_features.py` — two `cache_dir / key.json).write_text(...)`
  calls (lines 308, 358) plus a non-atomic `df.to_parquet(out_path)` at the
  final write (line 602).
- `konvid_to_full_features.py` — `cache_path.write_text(vmaf_json.read_text())`
  (line 274) and three `df.to_parquet(...)` calls in `_write_outputs`
  (lines 297, 307, 315).
- `extract_full_features.py` — `cache_path.write_text(json.dumps(payload))`
  (line 100) and `df.to_parquet(args.out)` (line 232).
- `aggregate_corpora.py` — output JSONL opened as `"w"` before any rows are
  written; a crash during iteration leaves a truncated file.
- `aiutils/run_manifest.py:write_manifest_json` — `path.write_text(...)` used
  by every script's provenance manifest; a crash during the write corrupts the
  manifest sidecar.

`extract_k150k_features.py` already has a correct atomic-parquet pattern
(`_write_parquet_from_rows` using `.tmp` + `tmp.rename`); the fix brings the
remaining scripts to the same standard.

## Decision

We will add `write_text_atomic(path, text)` to `aiutils/file_utils.py`,
implement it as `tempfile.mkstemp(dir=path.parent)` + write + `os.replace`,
and use it in all AI-script cache and output write sites that previously used
`Path.write_text()` directly. Final Parquet outputs in `bvi_dvc_to_full_features`,
`konvid_to_full_features`, and `extract_full_features` are migrated to
`write_parquet_atomic` (which already implements the tmp-rename pattern).
`write_manifest_json` in `aiutils/run_manifest.py` is also made atomic via the
same helper. The `aggregate_corpora.py` JSONL write is wrapped with
`tempfile.mkstemp` + `os.replace` inline.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Catch `json.JSONDecodeError` in cache-read and delete corrupt entry | Recovers from corruption at read time | Does not prevent corruption; adds logic at every read site | Treats a symptom, not the cause |
| Use `fcntl.flock` to mark files in-progress | Works on most Linux filesystems | Complex locking semantics; still does not prevent partial writes on kill | Over-engineered for a single-writer pipeline |
| Keep existing behaviour and rely on operator to delete corrupt files | Zero code change | Silently corrupts extraction runs; production impact | Unacceptable data-quality risk |

## Consequences

- **Positive**: cache-file corruption on interrupt is eliminated; resume after
  kill/OOM is safe; all final outputs are either old-or-new, never partial.
- **Negative**: requires `path.parent` to be on the same filesystem as the
  temp file (guaranteed by `dir=path.parent` in `mkstemp`); a very small
  extra inode allocation per write.
- **Neutral / follow-ups**: `write_text_atomic` exported from `aiutils` so
  future scripts can adopt it without copying the pattern.

## References

- `extract_k150k_features.py` existing atomic pattern: `_write_parquet_from_rows`
  (same-directory tmp + rename, plus `_fsync_path` before staging unlink).
- `aiutils/parquet_utils.py:write_parquet_atomic` — already implements atomic
  rename for parquet outputs.
- req: "ai-script-resume-correctness — checkpoint/resume correctness; partial-write
  file corruption protection; atomic file operations for state files"
