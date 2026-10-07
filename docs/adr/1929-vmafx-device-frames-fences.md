<!-- markdownlint-disable MD013 MD060 -->
# ADR-1929: VMAFx device frames, fences and the import rule: the shared contract the backend lanes implement

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (popups 2026-10-06); RC4 work package 3 (common lane)
- **Tags**: api, abi, rc4, gpu, cuda, sycl, hip, metal

## Context

[ADR-1829](1829-rc4-zero-copy-import.md) puts the device-memory import API
into RC4, and [ADR-1852](1852-vmafx-api-redesign.md) with its design review
([Research-2158](../research/2158-vmafx-api-redesign.md), section 2.7) gives
its shape: devices, a fence type, an import descriptor, release fences, frame
pools, admission, and decision D8 (one retry after a host wait on the acquire
fence, then a failure that names the import, never a silent host copy).
ADR-1880 (PR #2185) adds that one
frame is scored by several contexts without a second import, and that
capability structs stay size-prefixed so the RC6 / RC7 tables append each
device's format envelope.

RC4 work package 3 lands as one shared lane on the CPU device first, then one
lane per backend (CUDA, SYCL, HIP, Metal). The shared lane fixes every type,
function and rule the backend lanes fill in, so they add implementations and
never change the ABI. Implementing it left choices the design does not make:
how a library-owned host fence behaves, what a non-blocking fence query
returns, how the zero-copy default survives initialisers that zero every
field, what the CPU device does with an acquire fence it has no queue for,
and what the D8 helper looks like. They are recorded here before the ABI
freezes ([ADR-1897](1897-vmafx-abi-0x-numbering.md)).

## Decision

We implement device frames and fences with these rules.

1. **Every kind is declared now.** `VmafxMemoryKind` (HOST, DEVICE_POINTER,
   DEVICE_ARRAY, DMABUF, METAL_SURFACE, METAL_TEXTURE, WIN32_SHARED) and
   `VmafxFenceKind` (NONE, HOST, CUDA_EVENT, HIP_EVENT, SYCL_EVENT,
   SYNC_FILE, METAL_SHARED_EVENT, WIN32_SHARED) are in the definition; this
   lane implements HOST memory and NONE / HOST fences, and every other kind is
   refused naming it (`VMAFX_E_NOTSUP`) until its backend lane lands.
   `VmafxImportPlane` (embedded three times in `VmafxFrameImport`, so it never
   grows) carries a `size` of the memory object beside handle, descriptor,
   plane index, offset, pitch and modifier, because external-memory imports
   of a dma-buf need the object's size.
2. **Host fences are library objects.** A HOST fence is a reference-counted
   flag set once; each `VmafxFence` the library returns carries one
   reference, which `vmafx_fence_destroy()` drops. `vmafx_fence_create()`
   makes one for a producer to signal (`vmafx_fence_signal()`).
3. **A poll is an answer.** `vmafx_fence_wait()` with a timeout of 0 on an
   unsignalled fence returns `VMAFX_PENDING`, with no error and no log line
   ([ADR-1906](1906-vmafx-core-api-semantics.md) item 2); a wait with a
   timeout that expires is `VMAFX_E_TIMEOUT` (new status -10) naming the
   fence. Waiting polls with short sleeps against a monotonic clock, because
   the portable pthread subset has no timed condition wait.
4. **The release fence extends the frame rule.** ADR-1906 item 4 (one frame
   reference is one count of the engine picture's counter) applies to
   imported and pooled frames. `vmafx_frame_release_fence()` returns a fence
   signalled where the frame's last count is dropped, in whichever context or
   caller drops it, so a frame imported once and submitted to several
   contexts is released after the last reader of any of them. Host frames
   have release fences too.
5. **Zero copy is the zero value.** The `*_INIT` macros zero every field but
   `struct_size`, so the flag that relaxes the rule is
   `VMAFX_IMPORT_ALLOW_COPY`: a zeroed descriptor requires zero copy, the
   design's `VMAFX_IMPORT_REQUIRE_ZERO_COPY` default spelled as its inverse.
   Even with the flag a copy is a device copy; no path copies device memory
   through the host.
6. **NV12, P010 and P016 are converted, nothing else.** The planar frame an
   import makes holds the producer's planes where they are planar and
   unshifted, and the de-interleaved (and for P010 shifted by 6,
   [ADR-1679](1679-metal-iosurface-biplanar-import.md)) planes otherwise. The
   CPU device converts with the row readers of the Metal import
   (`core/src/metal/iosurface_layout.h`), so the reference the device kernels
   are held to is one implementation. The new pixel format values (16, 17,
   18) are import-only and leave room below for planar formats that mirror
   libvmaf's.
7. **The CPU device has no queue for a wait.** An acquire fence it cannot
   honour yet is `VMAFX_E_BUSY` from `vmafx_frame_import()`; the backend
   lanes enqueue a device-side wait instead.
8. **The import rule is one helper.** `vmafx_context_import_frame(context,
   device, desc, input, ...)` imports and runs `vmafx_context_admit()`; a
   `VMAFX_E_BUSY` or `VMAFX_E_TIMEOUT` is retried once after a host wait on
   the acquire fence of at most the context's `import_retry_wait_ns`
   (`VmafxContextConfig`, 1 ns to 10 minutes, a larger value refused with
   `VMAFX_E_RANGE` rather than clamped; 0, the initialiser's value, is the
   default of 10 seconds); a second failure, or any other,
   fails with one message naming the input, backend, device, memory kind,
   pixel format, depth, size, every plane's modifier, the cause and the
   number of attempts. It never clears the zero-copy rule.
9. **Admission generalises ADR-1688.** Before a frame is counted every
   registered extractor is checked against the memory the frame is in: a host
   frame is read by CPU extractors and uploaded by device twins; a frame in
   device memory is refused by a CPU extractor (it would need a host copy)
   and by a twin of another backend, and each refusing extractor is named
   with its reason. Whether a twin of the frame's backend reads it is the
   backend lane's per-extractor answer (the generalisation of
   `reads_shared_luma_only()`). Both inputs of a submit live in the same
   memory, and a device frame lives on the context's device.
10. **A context has one device, attached first.**
    `vmafx_context_use_device()` comes before any feature, model or frame,
    because the context picks each feature's twin when it is registered; the
    context holds a reference until it is destroyed.
11. **Pools are counted like frames.** A pool frame returns to its pool where
    its last count is dropped and signals its release fence there;
    `vmafx_frame_pool_destroy()` with frames in use frees nothing they need.
    Pool exhaustion is `VMAFX_E_BUSY`.
12. **Device information grows at the end.** `VmafxDeviceInfo` is
    size-prefixed and carries backend, index, creation flags, the memory and
    fence kinds the device imports, its memory and its name; no format
    envelope field is written by hand (ADR-1880 (PR #2185)).
13. **Test-only switches and counters live in the library.**
    `core/src/vmafx/frame_import_hooks.h` (hidden symbols, reached by the
    white-box tests through the static library) counts host copies and
    conversions and plants the defects the fence tests must catch: a skipped
    acquire wait, a release signalled when the submit returns, a host copy,
    a transient import failure and a device residency. The backend lanes
    assert the host-copy count stays 0.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Declare every fence and memory kind now (chosen) | Backend lanes fill in implementations without an ABI change or a definition conflict between lanes | Kinds exist that this build refuses | Chosen: the lanes run in parallel and the ABI freezes at `v1.0.0` |
| Each backend lane adds its kinds | Nothing is declared before it works | Four lanes edit the same enums and structs in parallel; each addition is a definition conflict and an ABI patch bump | Not chosen |
| Poll answers `VMAFX_PENDING` (chosen) | A producer polling release fences gets no ERROR line per poll; matches ADR-1906 item 2 | A wait returns two "not yet" statuses depending on the timeout | Chosen |
| Poll answers `VMAFX_E_TIMEOUT` | One status for "not signalled" | With a `NULL` error every poll writes an ERROR line (ADR-1906: no failure is silent) | Not chosen |
| `VMAFX_IMPORT_ALLOW_COPY`, zero means zero copy (chosen) | A zeroed or `*_INIT` descriptor is the safe default without per-field initialisers | Differs in spelling from the design's `VMAFX_IMPORT_REQUIRE_ZERO_COPY` | Chosen: the generator's initialisers set only `struct_size` |
| `VMAFX_IMPORT_REQUIRE_ZERO_COPY` (the design's spelling, Research-2158 section 2.7), set by the initialiser | The design's name | Needs a per-field initialiser in the generator; a descriptor zeroed by hand silently allows copies | Not chosen (maintainer: "ALLOW_COPY opt-in") |
| CPU import refuses an unsignalled acquire fence with `VMAFX_E_BUSY` (chosen) | The import never blocks; the D8 helper owns the one host wait | A raw import on the CPU needs the caller or the helper to wait | Chosen |
| CPU import waits on the fence | A raw import just works | A blocking call with no timeout in the descriptor, or an arbitrary internal one | Not chosen |
| CPU import keeps the fence and waits at submit | The import never blocks | Every reader of every context waits, and a semi-planar conversion at import would read unwritten planes | Not chosen |
| D8 as one helper on the context (chosen) | One rule for every filter and caller, admission included | The helper needs the context to name refusing extractors | Chosen |
| D8 left to each filter | No new function | Each filter writes the retry and the message; the rule drifts | Not chosen |
| D8 wait: a context option, default 10 s (chosen) | A caller with a slow producer or a tight deadline sets it once; the default needs no code | One more config field | Chosen by the maintainer |
| D8 wait: a library constant of 10 s | No API | A caller whose producer needs longer, or who must fail faster, cannot change it | Not chosen |
| D8 wait: a parameter of each import call | Per frame | Every call site repeats the value; no caller needs it per frame | Not chosen |
| D8 wait out of range: refused (chosen) | A typo (seconds for nanoseconds) fails at context creation, naming the field | A caller must know the bound | Chosen |
| D8 wait out of range: clamped | Never fails | A wrong unit silently becomes 10 minutes | Not chosen |
| Reuse the Metal row readers for the CPU conversion (chosen) | One reference implementation of de-interleave and shift (HISS-19) | A backend-named header used by the CPU path | Chosen; RC5 deduplication may rename it |

## Consequences

- **Positive**: the backend lanes implement the declared kinds behind fixed
  functions; every lane's tests can assert the same host-copy counter, fence
  ordering and admission messages; an imported frame scores bit for bit as
  the host frame on the CPU device (fixture and synthetic 4K cases).
- **Negative**: the CPU fence wait polls (50 microsecond sleeps on POSIX,
  1 millisecond on Windows); the D8 wait bound is per context, not per call;
  the test-only switches and the virtual test clock ship in the library
  (hidden, process-wide atomics).
- **Neutral / follow-ups**: the backend lanes add device creation, their
  memory and fence kinds, the per-extractor admission answer and the
  `sync_file` minimum kernel version; the generator may gain named `u64`
  constants for "wait without a limit" (request WP1-3). Found on the way:
  `VmafxError` truncated a subject at 95 bytes, so a long path was named
  wrongly (fixed: 1023 bytes, `T-VMAFX-ERROR-SUBJECT-TRUNCATED-2026-10-06`).

## References

- [ADR-1829](1829-rc4-zero-copy-import.md), [ADR-1852](1852-vmafx-api-redesign.md) decision D8, ADR-1880 (PR #2185), [ADR-1906](1906-vmafx-core-api-semantics.md), [ADR-1688](1688-sycl-zero-copy-luma-only-admission.md), [ADR-1679](1679-metal-iosurface-biplanar-import.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md), [Research-2158](../research/2158-vmafx-api-redesign.md) section 2.7.
- `Q` (maintainer popup 2026-10-05, D8): "One retry, then fail named (Recommended)".
- `Q` (maintainer popup 2026-10-06, zero-copy flag): "ALLOW_COPY opt-in (Recommended)".
- `Q` (maintainer popup 2026-10-06, D8 wait): "10 s default, per-context option (Recommended)".
- `Q` (maintainer popup 2026-10-06, poll and CPU acquire statuses): "Accept both (Recommended)".
- `req` (RC4 work package 3 brief, 2026-10-06): "The D8 rule: one retry after a host wait on the acquire fence, then fail naming backend / input / format / plane / refusing extractors, never a silent host copy. Provide a test-only host-copy counter the backend lanes assert stays 0."
- `req` (same brief): "Fence kinds cover CUDA/HIP/SYCL events, sync_file, Metal shared event and Win32 shared handles, even though only HOST and NONE are implemented in this CPU lane; backend lanes fill the rest."
- Tests: `core/test/test_vmafx_import_api.c`, `test_vmafx_import_bitexact.c`, `test_vmafx_import_fence.c`.
