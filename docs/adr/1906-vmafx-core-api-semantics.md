<!-- markdownlint-disable MD013 MD060 -->
# ADR-1906: VMAFx core API semantics: per-context logging, size negotiation, frame and model references

- **Status**: Proposed
- **Date**: 2026-10-05
- **Deciders**: RC4 work package 2 (core API); maintainer review on the draft pull request
- **Tags**: api, abi, rc4, logging

## Context

[ADR-1852](1852-vmafx-api-redesign.md) and its design review
([Research-2158](../research/2158-vmafx-api-redesign.md), sections 2.2 to
2.6) define the VMAFx core API: contexts with a per-context log callback,
options, models, refcounted frames, submission and synchronous scores, errors
that name what failed, and size-prefixed structs. Implementing them on the
existing engine (`core/src/libvmaf.c`) left choices the design does not make:
the engine logs through one process-wide function without a context; it
counts picture references in `VmafPicture`; it mounts models on the feature
collector with owners of its own ([ADR-1755](1755-collector-owns-mounted-model.md));
and libvmaf's model and dictionary objects are mutable. The choices below
decide what a caller can rely on, so they are recorded before the ABI freezes.

## Decision

We implement the core API with these semantics.

1. **Logging.** A context with a log callback receives, as one line each, the
   messages at or below its `log_level` that the engine raises on the calling
   thread during this context's calls (a thread-local sink in
   `core/src/log.cpp`, installed by the VMAFx layer around every engine call
   and restored after it), and every failure reported without an error
   out-parameter. It never changes the process log level. Messages raised on
   the engine's worker threads go to the process log. A context without a
   callback logs to the process log and sets its level, as `vmaf_init()` does.
2. **Failures and pending scores.** A failure reported without an error
   out-parameter is delivered at `ERROR` whatever the log level (to the
   callback, else stderr). `VMAFX_PENDING` is an answer, not a failure: no
   error, no log line, the output struct untouched.
3. **Struct sizes.** An input struct whose `struct_size` is below the size the
   struct had when it was introduced is `VMAFX_E_ABI` (-11), naming the
   struct; fields past the caller's size take their defaults, fields past ours
   are ignored. Output structs accept any `struct_size` of at least 4 and
   receive the prefix it covers. The introduction sizes are one table in
   `core/src/vmafx/internal.h`.
4. **Frames.** One frame reference is one count of the engine picture's
   reference counter. `vmafx_submit()` moves the caller's two counts into the
   engine on every path, so a frame held by the engine as frame n-1 or n-2
   (ADR-1478) and a frame held by a caller are the same kind of reference,
   and one frame submitted to several contexts (one reference each) is never
   copied (ADR-1880, PR #2185). The same
   frame as both inputs of one submit needs two references and is refused
   otherwise. Borrowed host planes are released by a callback on the thread
   that drops the last reference.
5. **Models.** A `VmafxModel` wraps the engine model with its own reference
   count and holds one engine owner. A context holds a reference to every
   model and model set it uses and drops them only after the engine close
   succeeded, so a failed `vmafx_context_destroy()` keeps them (ADR-1336
   retry contract). `vmafx_model_override_feature()` is allowed only while the
   caller holds the only reference (`VMAFX_E_BUSY` once shared), which makes a
   model a context may use immutable. A model set's lead model is borrowed
   from the set. The model hash is the SHA-256 of the bytes as loaded.
6. **Engine entry points.** The libvmaf bodies the core API calls are renamed
   `vmaf_engine_*` (`core/src/vmafx/engine.h`); the libvmaf names forward to
   them until the compat layer generates them as shims on the VMAFx API, and
   engine code calls only the `vmaf_engine_` names, so that change cannot
   recurse.
7. **Feature resolution.** `vmafx_feature_resolve()` on a context without a
   device backend answers with the CPU extractor (`VMAFX_OK`) rather than the
   `-ENODEV` of `vmaf_feature_backend_twin()`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Logging: thread-local sink around engine calls (chosen) | Per-context routing of every engine message raised during the context's calls with one hook in `vmaf_log()`; no change to any extractor; contexts on different threads log independently | Messages raised on the engine's worker threads still go to the process log | Chosen: covers the messages that explain a failed call; worker messages are rare and still visible |
| Logging: pass the context into every engine log call | Complete routing, worker threads included | Touches every `vmaf_log()` call site in the engine and the extractors, upstream-mirror files included; large rebase surface | Not chosen for RC4; possible later once the engine carries a context per extractor |
| Logging: route only the VMAFx layer's own messages | No engine change | Engine messages (unknown option, model load) still go to stderr at the process level; a context with a callback would still change the process level | Not chosen: does not replace the process-global level for VMAFx users |
| Struct sizes: refuse every size other than ours | Simplest | Breaks every caller compiled against older headers at the first field added | Not chosen: contradicts design section 2.6 |
| Frames: a reference count of the frame object separate from the picture's | Frame object lifetime independent of the engine | Two counters for one lifetime; the engine's n-1 / n-2 references would not keep the frame object, so borrowed planes could be released while the engine reads them | Not chosen |
| Models: immutable after load, no override | Strictest | The CLI and the FFmpeg filters override model options (`model=...:name=...:feature=...`); device-targeted scoring sets `adm_norm_view_dist` per context (ADR-1880) | Not chosen: override while unshared covers both |

## Consequences

- **Positive**: callers get named failures through a callback without a
  process-wide log level; frames, models and sets have one reference rule
  each; the ABI check plus the size table make additive struct growth safe.
- **Negative**: worker-thread engine messages bypass the callback; the
  minimum-size table is maintained by hand until the generator emits it
  (request WP1-1 under the RC4 work-package requests).
- **Neutral / follow-ups**: the compat layer (RC4 WP6) turns the forwarding
  libvmaf functions into generated shims and renames the `core/src/model.c`
  and `core/src/dict.cpp` bodies the core API calls; device frames and fences
  (WP3) extend the frame rule to imported memory.

## References

- [ADR-1852](1852-vmafx-api-redesign.md), [Research-2158](../research/2158-vmafx-api-redesign.md) sections 2.2 to 2.6.
- `req` (RC4 work package 2 brief, 2026-10-05): "config with per-context log callback (replaces the process-global level for VMAFx users; compat keeps the global)"; "Refcounted: context holds a reference (ADR-1755)"; "One frame, many contexts (ADR-1880, RC5 device targets): a `VmafxFrame` is refcounted across contexts; scoring it in two contexts with different options needs no copy and gives each context the score a separate run gives."
- Tests: `core/test/test_vmafx_context.c`, `test_vmafx_frame.c`, `test_vmafx_model.c`, `test_vmafx_lifetime.c`.
