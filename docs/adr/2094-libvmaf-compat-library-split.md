<!-- markdownlint-disable MD013 MD060 -->
# ADR-2094: libvmaf.so.3 is a compat library on the exported VMAFx API; the engine compiles its libvmaf bodies under engine names, and backends keep theirs until their lanes land

- **Status**: Proposed
- **Date**: 2026-10-06
- **Deciders**: RC4 work package 6
- **Tags**: api, rc4, abi, build, compat, ffmpeg

## Context

[ADR-1852](1852-vmafx-api-redesign.md) decision D3 splits the library:
`libvmafx.so.1` holds the engine and the VMAFx API and exports `vmafx_*`
only; a thin `libvmaf.so.3` implements the 107 exported libvmaf functions
on exported `vmafx_*` symbols only, so a libvmaf function the new API cannot
express fails the link. Design section 2.11 lists every function with its
`vmafx_` target. Carrying it out leaves choices the design does not settle:

- The engine defines the libvmaf functions today (`core/src/libvmaf.c`,
  `model.c`, `picture.c`, `dnn/`, `mcp/` and the backend runtimes) and calls
  them from about seventy translation units. A static link of `libvmaf.a`
  and `libvmafx.a` must not bind an engine call to a compat shim (which
  calls back into the engine: recursion) or meet two definitions of one
  name. Work package 2 renamed eighteen bodies by hand and left forwarders.
- The CUDA, SYCL, HIP and Metal libvmaf functions need device frames of the
  new API. Only the CUDA lane of work package 3 is in review (#2277); the
  others have not started. CPU builds export HIP and Metal stub functions.
- `vmaf_preallocate_pictures()` on a context with an imported CUDA or SYCL
  state allocates page-locked host pictures, and the ADR-1478 check refuses
  a pool too small for an extractor that reads frame n-2, also when such an
  extractor registers later. The design's target, `vmafx_frame_pool_create()`
  on the CPU device, knows neither.
- A libvmaf picture (`VmafPicture`) is a caller-visible struct whose
  reference count the engine owns; the compat library cannot reach it.

## Decision

1. **Engine names by a forced header.** Every C / C++ translation unit is
   compiled with the generated `core/src/vmafx/engine_names_gen.h` first
   (`add_project_arguments`, `-include` / `/FI`; Objective-C++ through the
   Metal target's arguments). It defines `vmaf_<stem>` as
   `vmaf_engine_<stem>` for every libvmaf function the compat library
   defines, so the public headers declare, and the engine sources define and
   call, the engine names; the sources keep their libvmaf spelling. Targets
   that use the libvmaf API (the compat library, the tools, black-box tests)
   define `VMAF_PUBLIC_NAMES`. `libvmafx.so.1` links the generated version
   script with `local: *;` (`hide_unlisted = true`).
2. **Backend exceptions in the definition.** A `[[compat]]` entry of kind
   `engine` (CUDA 5, SYCL 20) has no compat definition: in a build with its
   backend the engine keeps the function and exports it from
   `libvmafx.so.1` in the version node `VMAF_LEGACY_<BACKEND>`
   (`core/src/vmafx_legacy_<backend>.map`); `until` names the lane that ends
   it. A `manual` entry with `engine_with` (HIP 5, Metal 9) is a compat
   function on the new API in builds without that backend and the engine's
   own in builds with it. The export checks and the conformance test read
   these conditions from the generated lists.
3. **Context-owned frames.** `vmafx_context_preallocate()` and
   `vmafx_context_acquire_frame()` let the context allocate frames in the
   memory its device reads fastest, through the engine's own pool; each
   frame handed out adopts the pool picture. The CUDA / SYCL page-locked
   pools and the retention check keep working.
4. **A libvmaf picture is a view of a frame.** `vmafx_frame_from_picture()`
   and `vmafx_frame_to_picture()` (bridge header) each take a new reference;
   an engine picture without a frame is adopted by a frame object that takes
   over its release and restores it on the last unref. Models and model sets
   map to their libvmaf handles through `api_owner` back pointers.
5. **Conformance by two tables.** A generated table of the compat functions
   is compiled twice, with the engine names and with the libvmaf names; every
   scenario of `test_compat_conformance` runs through both and the traces
   (return values, outputs, every score as `%a`) must be equal, and every
   compat function of the build must have been called.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Rename every body in the source (`vmaf_engine_*` in about seventy files) | Names in the source match the binary | Large diff across upstream-mirror files; every upstream sync conflicts on the renamed lines; an internal header must declare about ninety functions | The forced header gives the same binary with the upstream spelling untouched |
| Partial linking (`ld -r` + `objcopy --localize-hidden`) of the static engine | No compile-time renaming | Not available with the MSVC toolchain; two static archives still define the libvmaf names for white-box tests | Windows builds are static (ADR-0121) |
| Write the GPU compat functions now on the common device API | Removes the exceptions at once | CUDA device frames are in #2277, SYCL / HIP / Metal lanes have not started; the code would be untestable and broken until they land | Exceptions are named in the definition, checked by the export tests and end with each lane |
| `vmafx_frame_pool_create()` on the CPU device for `vmaf_preallocate_pictures()` (design) | One pool concept | Loses page-locked pools of imported GPU states and the ADR-1478 refusal at registration; non-blocking acquire where libvmaf waits | Kept the engine pool behind a context function; WP3 lanes can move it onto device pools |
| Compat library keeps a private copy of engine helpers (reference counts, pools) | Fewer API additions | The compat library would link engine symbols, defeating the completeness proof of D3 | D3's point is the link-time proof |

## Consequences

- **Positive**: `libvmafx.so.1` exports 0 libvmaf symbols in a CPU build and
  `libvmaf.so.3` exactly the 74 libvmaf functions of that build; a compat
  source that reaches past the exported API does not link. The golden gate,
  upstream FFmpeg's `libvmaf` filter and upstream GStreamer's `vmaf` element
  run through the compat library. Upstream syncs port into the engine
  sources unchanged.
- **Negative**: the binary names differ from the source names inside the
  engine (`vmaf_engine_<stem>`), which a debugger shows; a translation unit
  built without the header would define libvmaf names again (the export
  checks and the conformance link catch it). A Windows shared build still
  exports engine names through `__declspec(dllexport)` until WP12 links
  `vmafx.def`, and a macOS one through default visibility until WP12 adds
  the export list. Deliberate differences of the compat layer from libvmaf are
  listed in `docs/api/vmafx/index.md` (an invalid report format no longer
  truncates the file first; options for an extractor the model does not read
  are accepted but not recorded in provenance; changing a model a context
  holds is refused).
- **Neutral / follow-ups**: each WP3 lane turns its backend's `engine`
  entries into `manual` compat functions and deletes its legacy map. This
  change generates `libvmafx.pc` and `libvmaf.pc` (`Requires: libvmafx`) and
  copies `libvmafx.so*` into the container images; WP12 ships the split in
  the release artifacts (`scripts/release/build-native-release-artifacts.sh`
  and `supply-chain.yml` still stage `libvmaf.so*` only), the install tests,
  the darwin export list and the Windows `.def`. Two libvmaf functions newer
  than this branch's base need entries when the RC4 chain moves onto master:
  `vmaf_set_sample_range_check_enabled()` (#2221, on master) as a context
  setting of the VMAFx API, and `vmaf_set_input_colorimetry()` (#2300,
  ADR-2093) on the colour VMAFx frames carry: `VmafxFrameDesc` grows a
  `VmafxColor` at its end, a context function gives the colour of frames that
  carry none (the compat function calls it, so it needs no state of its own),
  and the submit hands each pair's colour to the engine's conversion state,
  `VMAFX_E_BUSY` for a change after the first converted pair as libvmaf's
  `-EBUSY`; a conformance scenario with a `conversion_target` model covers it.
  `test_libvmaf_deprecation` fails on a header function without an entry, so
  neither can be missed. Deprecation warnings stay opt-in
  (`VMAF_ENABLE_DEPRECATION_WARNINGS`, D7).

## References

- `req` (work package brief, 2026-10-06): "the 107 `libvmaf` functions on the new API, the library split into libvmafx.so.1 + a thin libvmaf.so.3 on exported vmafx_ symbols only per ADR-1852 D3, conformance tests, the upstream-FFmpeg job"; "SYCL / HIP / Metal compat functions may stay on their current implementations until their WP3 lanes land, but say so"; "WP6 must rename the model.c / dict.cpp bodies the VMAFx API calls before turning them into shims, or calls recurse".
- [ADR-1852](1852-vmafx-api-redesign.md) decisions D2, D3, D7 and design section 2.11; [ADR-1897](1897-vmafx-abi-0x-numbering.md); [ADR-1906](1906-vmafx-core-api-semantics.md); [ADR-1929](1929-vmafx-device-frames-fences.md); [ADR-2073](2073-vmafx-provenance-record.md); [ADR-1478](1478-motion-five-frame-window-port.md); [ADR-0121](0121-windows-gpu-build-only-legs.md); issue #2237 (upstream GStreamer element conformance).
