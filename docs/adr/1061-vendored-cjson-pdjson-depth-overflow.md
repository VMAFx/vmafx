<!-- markdownlint-disable MD013 MD060 -->
# ADR-1061: Fix depth-limit, integer-overflow, and banned-function bugs in vendored pdjson and cJSON

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: lusoris
- **Tags**: security, vendored, mcp, c, libvmaf, fork-local

## Context

Two vendored JSON libraries ship with the fork:

- **pdjson** (`core/src/pdjson.c`) — a streaming pull parser used by
  `read_json_model.c` to load VMAF model files.
- **cJSON** (`core/src/mcp/3rdparty/cJSON/`) — a DOM-style parser/generator
  used by the MCP server (`core/src/mcp/`) for JSON-RPC framing.

A security audit (r11 round) found five concrete bugs across the two files:

### pdjson — three bugs

### Bug 1 — No depth limit (OOM via unbounded stack growth)

`push()` in `pdjson.c` contains a depth guard at lines 56–60, but it is
wrapped in `#ifdef PDJSON_STACK_MAX`. `PDJSON_STACK_MAX` was never defined
anywhere in the build system, so the guard compiled to nothing. A crafted
deeply-nested JSON input (model file or MCP message) triggers unlimited
`realloc` growth on the `json->stack` array, eventually exhausting heap
memory.

### Bug 2 — Integer overflow in `push()` size calculation (CERT-C INT30-C)

Line 65 (before fix): `size_t size = (json->stack_size + PDJSON_STACK_INC) * sizeof(*json->stack);`
had no guard against the addition wrapping when `stack_size` was near
`SIZE_MAX / sizeof(struct json_stack)`. The multiply would then produce a
value smaller than the current allocation, causing `realloc` to shrink
the buffer while the code continued writing to the old (larger) region.

### Bug 3 — Integer overflow in `pushchar()` size doubling (CERT-C INT30-C)

Line 157 (before fix): `size_t size = json->data.string_size * 2;` — when
`string_size >= SIZE_MAX / 2` the multiply wraps to zero or a small value.
On glibc, `realloc(ptr, 0)` frees and returns NULL (error path fires
correctly), but on other platforms it may return a non-NULL stub pointer.
The subsequent write at line 167 would then be an out-of-bounds store.

### cJSON — two bugs

### Bug 4 — Banned functions not yet remediated (ADR-0683)

ADR-0683 (Accepted 2026-05-22) mandated replacing all `sprintf` and `strcpy`
call sites in `cJSON.c`. Two prior PRs (#890, #891) were opened and then
closed without merging. The violations (12 call sites) remained in the tree
and continued to fail the fork's lint gate on every file-touch.

Sites replaced in this PR:

| Line (before) | Original call | Replacement |
|---|---|---|
| 125 | `sprintf(version, ...)` | `snprintf(version, sizeof(version), ...)` |
| 383 | `strcpy(object->valuestring, valuestring)` | `memcpy(..., strlen(valuestring)+1)` |
| 515, 517, 520, 526 | `sprintf(number_buffer, ...)` | `snprintf(number_buffer, sizeof(number_buffer), ...)` |
| 833 | `strcpy(output, "\"\"")` | `memcpy(output, "\"\"", sizeof("\"\""))` |
| 910 | `sprintf(output_pointer, "u%04x", ...)` | `snprintf(..., 6, "u%04x", ...)` |
| 1266, 1274, 1282 | `strcpy(output, "null"/"false"/"true")` | `memcpy(output, ..., sizeof(...))` |

### Bug 5 — `cJSON_GetArraySize` wraps `size_t` to `int` (CERT-C INT31-C)

`cJSON_GetArraySize` counts array children into a `size_t` variable and
then casts the result to `int` without any bound check. For arrays larger
than `INT_MAX` elements the cast wraps, returning a negative value to the
caller. The upstream file contained a `/* FIXME: Can overflow here */`
comment. This PR clamps the value to `INT_MAX` (the same saturation
strategy used by `ensure()` in the same file for `needed > INT_MAX`).

## Decision

Apply all five fixes to the live vendored sources:

1. **pdjson depth limit**: define `PDJSON_STACK_MAX 512` unconditionally
   at the top of `pdjson.c`, before the `#ifdef` guard. 512 levels is far
   beyond any legitimate VMAF model or MCP message depth.
2. **pdjson `push()` overflow**: add explicit addition-overflow and
   multiply-overflow guards before the `realloc` call.
3. **pdjson `pushchar()` overflow**: add a `string_size > SIZE_MAX/2` guard
   before the `* 2` doubling.
4. **cJSON banned functions**: replace `sprintf` → `snprintf` (with explicit
   buffer size) and `strcpy` → `memcpy` (with `sizeof` or `strlen+1` size)
   at all 12 call sites identified by ADR-0683.
5. **cJSON `GetArraySize` clamp**: add `if (size > (size_t)INT_MAX) return INT_MAX;`
   before the `(int)size` cast.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Apply `// NOLINT` to banned-function sites | Zero diff | Forbidden by ADR-0278 (NOLINT requires inline ADR for load-bearing invariants); `feedback_vendored_in_scope` rule explicitly prohibits this | Not chosen |
| Sync cJSON to a newer upstream release | Picks up any upstream improvements | cJSON upstream still uses `sprintf` in some paths; introduces unreviewed churn beyond the target scope | Not chosen as primary strategy; sync is the recommended future path but banned-function fixes must land now |
| Set `PDJSON_STACK_MAX` via Meson `-D` compile flag | Avoids modifying vendored file | The guard is `#ifdef`, not `#if`; any value (including 0) activates it — but the intent is to bake in a safe default unconditionally | Compile-flag approach still requires a source-level fallback default; consolidating in the source is cleaner |

## Consequences

- **Positive**: pdjson is safe against crafted deeply-nested inputs;
  `pushchar()` and `push()` are overflow-clean (CERT-C INT30-C satisfied);
  cJSON.c passes the fork's banned-function lint gate (ADR-0683 finally
  implemented); `cJSON_GetArraySize` no longer wraps on large arrays.
- **Negative**: Minor deviation from upstream vendored sources; future
  version syncs must re-verify and re-apply these fixes.
- **Neutral**: The depth limit of 512 is a soft policy decision; adjust
  `PDJSON_STACK_MAX` in `pdjson.c` if a legitimate model format requires
  deeper nesting.

## References

- `docs/principles.md` §1.2 rule 30 (banned functions).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file lint-clean rule.
- [ADR-0278](0278-nolint-citation-closeout.md) — NOLINT citation requirements.
- [ADR-0683](0683-cjson-banned-function-remediation.md) — prior decision mandating these cJSON fixes.
- CERT-C INT30-C: unsigned integer wraparound.
- CERT-C INT31-C: signed/unsigned conversion overflow.
- Prior closed PRs for ADR-0683: #890, #891.
