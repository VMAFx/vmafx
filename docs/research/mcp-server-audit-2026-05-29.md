# Research digest: MCP server audit (2026-05-29)

## Scope

Static audit of `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` and
`mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py` against five
concern categories: `isError` correctness, silent error swallowing,
resource leaks, schema/surface drift, and CLI/MCP feature-parity gaps.

---

## Finding 1 — `isError=False` on error (already fixed; confirm pattern holds)

**ADR-0608 comment at line 2161** documents that the previous design
returned `TextContent({"error": ...})` with `isError` implicitly False,
causing conformant MCP clients to misread errors as successes.  The fix
(raise from `_call_tool`, let the mcp library set `isError=True`) is
present at line 2149 and is correct.  No new violation was found in
this audit.

---

## Finding 2 — Dead first definition of `_run_benchmark` (lines 722–799)

`server.py` contains **two separate `async def _run_benchmark`
definitions**.  The first (line 722) lacks a `progress_token` parameter
and is fully shadowed at module load by the second (line 1302).  Python
silently replaces the name; the dead function is never reachable.  The
only effect is dead code that inflates the file and confuses reviewers.

---

## Finding 3 — `_send_progress` swallows all exceptions silently (line 1062)

```python
    except Exception:
        pass
```

A progress-notification failure (network error, broken pipe, serialisation
error) is discarded without logging.  The bare `pass` means:

- The server cannot detect when the MCP transport has died mid-run.
- Debugging a broken-pipe scenario produces no signal.

The `LookupError` branch above it (for out-of-context calls in tests) is
correct; only the bare `Exception` branch is the problem.  It should log
at DEBUG level at minimum.

---

## Finding 4 — `_list_extractors` hard-codes stale `libvmaf/` path (line 858)

```python
feature_dir = _repo_root() / "libvmaf" / "src" / "feature"
```

The fork was renamed from `libvmaf/` to `core/` in ADR-0700 (commit
`61ff5e0565`).  `libvmaf/src/feature` does not exist; `core/src/feature`
does.  The function silently finds zero `.c` files and returns an empty
list — a silent wrong-answer rather than an error.  Verified by running
`_repo_root() / "libvmaf" / "src" / "feature" → not found`.

Same stale path appears in `_vmaf_binary()` candidate list (line 86):

```python
_repo_root() / "libvmaf" / "build" / "tools" / "vmaf",
```

This is a non-critical fallback (the binary is also searched under
`/usr/local/bin/vmaf` and `build/tools/vmaf`) but is dead weight.

---

## Finding 5 — `subsample` parameter silently dropped in `vmaf_score_encoded`

`_run_vmaf_score_encoded` accepts `subsample: int` and its docstring says
it "passes `--frame_step` to vmaf".  However the `ScoreRequest` dataclass
has no `subsample` field, and neither `_run_vmaf_score` nor `_build_argv`
ever appends `--subsample <N>` to the argv list.  The CLI flag is
`--subsample` (confirmed in `core/tools/cli_parse.c` line 224).  Calling
`vmaf_score_encoded` with `subsample=10` silently scores every frame.

---

## Finding 6 — `_load_vlm` swallows VLM load errors silently (line 564)

```python
except Exception:  # pragma: no cover - depends on local env
    continue
```

If a VLM candidate model raises a non-`ImportError` at load time (OOM,
corrupted weights, transformers ABI mismatch) the loop continues to the
next candidate without any log entry.  The caller gets "(VLM unavailable)"
with no indication of *why*, making diagnosis impossible.  At minimum the
exception should be logged at WARNING level.

---

## Finding 7 — Schema drift: `vmaf_score` advertises `bitdepth=16` (unsupported)

The `vmaf_score` tool schema lists `"enum": [8, 10, 12, 16]` for
`bitdepth`.  The CLI (and `_PIXFMT_TO_FFMPEG`) only supports 8/10/12-bit.
`bitdepth=16` will reach vmaf and fail at runtime with a cryptic message.
The schema should be `"enum": [8, 10, 12]` to match libvmaf's actual
support.  `describe_worst_frames` has the same schema overpromise
(line 1887).

---

## Finding 8 — No `list_extractors` fallback when `core/` path is absent

`_list_extractors` catches `OSError` per-file (line 864) but does not
handle the case where `feature_dir` itself does not exist.  After fixing
Finding 4 (path rename), callers in CI containers that lack the source
tree will get an empty list with no error — acceptable — but the current
broken path returns the same empty list for a different reason, masking
the bug.

---

## Verdict

| # | Severity | Category | File | Line |
| --- | --- | --- | --- | --- |
| 2 | Medium | Dead code / silent shadow | server.py | 722 |
| 3 | Low | Error swallowing | server.py | 1062 |
| 4 | **High** | Wrong path → empty result | server.py | 858, 86 |
| 5 | **High** | CLI/MCP parity gap (subsample dropped) | server.py | 1720 |
| 6 | Low | Error swallowing | server.py | 564 |
| 7 | Medium | Schema drift (bitdepth=16 unsupported) | server.py | 1787, 1887 |
| 8 | Low | Silent empty on missing source tree | server.py | 843 |
