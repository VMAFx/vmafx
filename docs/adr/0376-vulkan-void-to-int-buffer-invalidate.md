<!-- markdownlint-disable MD013 MD060 -->
# ADR-0376: Fix silent error-swallow in Vulkan buffer-invalidate readback functions

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `gpu`, `build`, `correctness`, `fork-local`

## Context

GCC 16 promotes `-Wreturn-mismatch` to a hard error. This surfaced four sites in two
Vulkan feature-extractor files where `return <int-expr>;` appeared inside functions
declared `static void`:

- `core/src/feature/vulkan/float_ansnr_vulkan.c:299` — `return err_inv;`
  inside `static void reduce_partials(...)`
- `core/src/feature/vulkan/float_ansnr_vulkan.c:302` — same
- `core/src/feature/vulkan/cambi_vulkan.c:884` — `return err_inv_img;`
  inside `static void cambi_vk_readback_image(...)`
- `core/src/feature/vulkan/cambi_vulkan.c:904` — `return err_inv_mask;`
  inside `static void cambi_vk_readback_mask(...)`

These are not cosmetic type-system accidents. All four sites guard calls to
`vmaf_vulkan_buffer_invalidate()`, which performs a coherency flush that makes
GPU-written host-mapped memory visible to the CPU. If the flush fails and
execution continues into the pixel-copy loop, the CPU reads potentially stale
memory that the GPU has not committed to the coherency domain. The previous
`static void` signature caused the compiler to silently discard the returned
error code — the error was irretrievably lost and the function proceeded as if
the flush had succeeded.

The fix changes the behaviour, not merely the syntax: previously the call site
could never observe a buffer-invalidate failure; after this fix it can, and will
propagate it up the call chain as a frame-extraction error rather than silently
producing incorrect scores.

## Decision

Change the four affected functions from `static void` to `static int` and update
every call site to check the return value. The functions now return `0` on success
and the buffer-invalidate error code on failure. Call sites use the fork-standard
`if (err) return err;` pattern (for `cambi_vk_readback_{image,mask}`) and
`if (err) goto cleanup;` (for `reduce_partials`, which already uses that cleanup
label for symmetric resource teardown).

The `static void` → `static int` + `return 0` refactor is the minimal correct
shape. A `// NOLINT` suppression would not be correct here — the error really must
be propagated, not ignored.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `(void)` cast the return value — swallow intentionally | Silences the compiler | Does not fix the stale-data bug; makes the silent-swallow explicit and permanent | Rejected — the coherency flush must not fail silently |
| `return;` at the void-return sites — strip the int entirely | Preserves void signature | Also swallows the error; stale-data bug remains | Rejected — same reason |
| Log + zero-fill fallback on invalidate failure | Degrades gracefully; preserves frame count | Adds branching complexity; a cache-coherency failure is a driver-level fault where proceeding is likely to produce systematically wrong scores | Rejected — propagation is cleaner and consistent with the rest of the Vulkan error surface |

## Consequences

- **Positive**: buffer-invalidate failures are now propagated as frame-extraction
  errors rather than silently producing incorrect scores from stale host-mapped
  memory. Build is clean under GCC 16 (`-Wreturn-mismatch` hard error). No
  behavioural change on a functioning Vulkan driver (the flush succeeds and `err`
  is 0).
- **Negative**: a caller that previously received a score (potentially stale) will
  now receive an error and drop the frame. In practice, a buffer-invalidate failure
  is a driver-level fault that indicates the mapped memory should not be trusted, so
  this is the correct behaviour.

## References

- GCC 16 release notes: `-Wreturn-mismatch` promoted to hard error.
- `core/src/feature/vulkan/float_ansnr_vulkan.c` lines 294–313 (pre-fix),
  lines 367–376 (`reduce_partials` call site).
- `core/src/feature/vulkan/cambi_vulkan.c` lines 880–917 (pre-fix),
  lines 1266–1268 (call sites).
- ADR-0186 (Vulkan image-import implementation) — background on the Vulkan
  host-mapped buffer coherency model used by the fork.
- ADR-0354 (submit-pool migration) — context for the ANSNR Vulkan extractor
  buffer lifecycle.
