# Research-2073: FFmpeg n9.0.2 stable refresh

- **Status**: Active
- **Workstream**: [ADR-1240](../adr/1240-ffmpeg-release-patch-lifecycle.md)
- **Last updated**: 2026-09-20

## Question

Can the maintained FFmpeg baseline advance from `n9.0.1` to `n9.0.2`
without changing integration semantics or regenerating any of the fork's 18
existing integration patches, and can the image integration path complete
without carrying forward any warning or error exposed by that build?

## Sources

- The official [`n9.0.2` tag](https://github.com/FFmpeg/FFmpeg/tree/n9.0.2)
  peels to upstream commit
  [`946fcce07b6dcd0331c8cc609192aeff5e1924f8`](https://github.com/FFmpeg/FFmpeg/commit/946fcce07b6dcd0331c8cc609192aeff5e1924f8).
- The upstream
  [`n9.0.1...n9.0.2` comparison](https://github.com/FFmpeg/FFmpeg/compare/n9.0.1...n9.0.2)
  records the stable patch-release delta.
- The `n9.0.2`
  [`configure` source](https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/configure#L4799)
  defines the current `libnpp` option behavior.
- The repository's maintained replay tool is
  [`scripts/ci/ffmpeg_patch_stack.py`](../../scripts/ci/ffmpeg_patch_stack.py).
- Meson documents ordered language-standard preferences in its
  [1.3.0 release notes](https://mesonbuild.com/Release-notes-for-1-3-0.html)
  and the `c_std` / `cpp_std` behavior in its
  [built-in options reference](https://mesonbuild.com/Builtin-options.html).
- GCC documents numeric LTO partition parallelism under
  [`-flto=n`](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html#index-flto).

## Findings

- The scheduled-release refresh selected `n9.0.2` and resolved it to
  `946fcce07b6dcd0331c8cc609192aeff5e1924f8`.
- The existing 18 integration entries replayed cumulatively and remained
  byte-identical. Patch 0019 was then appended to fix the complete GCC 14/16
  diagnostic inventory found by production and FATE builds. All 19 entries
  replay on the official tag with no refresh drift; the resulting patched tree is
  `1fd76f79179a6a51b5bb773a89c5334c6324c755`.
- FFmpeg 9 has removed libnpp support. `n9.0.2` still parses
  `--enable-libnpp`, but only emits the warning that enabling it does nothing.
  The dev image should continue omitting the flag because it adds no filter and
  would violate the warning-clean configure contract; this is not a CUDA 13
  compatibility hard error.
- The first isolated node-image build did not reach FFmpeg. Its partial source
  context omitted `scripts/ci/check-msvc-clz-shim.sh`, although
  `core/test/meson.build` resolves that file during every configure. Both the
  node and Go-server Dockerfiles now copy the configure input explicitly.
- The same integration run exposed four warning-producing builder defects:
  the builders omitted `xxd`, which silently disabled built-in models and
  exposed constant-zero loop warnings; they omitted `make`, so GCC's
  `-flto=4` LTRANS work fell back to serial execution; plain `cp` flattened the
  libvmaf SONAME symlink chain; and the node image synthesized `libvmaf.pc`
  from the product version (`dev`) instead of the interface version (`3.0.0`),
  making FFmpeg's `libvmaf >= 2.0.0` probe fail. Both partial builders now
  install the required tools, preserve links with `cp -a`, and stage Meson's
  generated pkg-config file.
- `core/src/model.c` also compiled warning-free only while built-in models were
  present. The disabled configuration now excludes the table and its loops at
  preprocessing time. A `-Dwerror=true -Dbuilt_in_models=false` build and its
  `test_model` executable provide red/green coverage; two tests that loaded a
  built-in model unconditionally are now correctly gated, while a new disabled
  case pins the public empty-registry behavior. The generated CPU clang-tidy
  ratchet tightens `core/src/model.c` from eight findings to zero; its C-only
  `nullptr` false positives are scoped exactly as ADR-1138 requires, and the
  touched `core/test/test_model.c` translation unit remains at zero in both
  model configurations.
- Meson's two warnings about manually supplied language-standard flags were a
  deliberate consequence recorded by ADR-1056, but are no longer accepted.
  ADR-1273 replaces them with ordered built-in standard preferences and keeps
  an explicit `std::expected` compile probe.
- The first complete FFmpeg build emitted 70 GCC 14 diagnostics across
  array-bound proofs, implicit fallthroughs, possible format truncation, and
  stack frames as large as 327 KiB. Patch 0019 fixes each root cause: VVC stays
  enabled with scratch owned by `VVCLocalContext`; median and FFV1 scratch
  moves off stack; MPEG-TS snapshots copy only rollback state; fallthroughs
  are explicit; and bounded format/parser/table paths reject invalid or
  overlong input. DASH, SmoothStreaming, and RTSP now propagate the errors
  exposed while hardening those paths. The same GCC 14 configuration then
  compiled with zero warnings, including the VVC decoder.
- A subsequent complete FATE build compiled code that the production targets
  do not reach and exposed 27 additional GCC 14 stack-usage diagnostics in
  checkasm, APV, and CABAC test translation units. Patch 0019 moves those
  large scratch arrays and lookup tables to checked, aligned heap storage,
  retains their original reuse lifetime, and frees them on every exit path.
  The tests remain enabled and no diagnostic suppression is used.
- An independent GCC 16 production/FATE build exposed 18 additional
  diagnostics in AAC, license-gated ADPCM helpers, and SVQ1 after GCC 14 was
  clean. The AAC encoder now rejects impossible psychoacoustic window counts,
  HVQM helpers follow their decoder configuration, and SVQ1's recursive block
  level is a checked signed invariant.
- A clean GCC 16 `make build`, which compiles tools and test programs beyond
  the ordinary production target, exposed the final 11 compiler diagnostics:
  one missing no-return contract in `seek_print`, eight real path-truncation
  reports in `ismindex`, and two oversized test stacks in swresample and snow.
  Patch 0019 now declares the exit contract, builds arbitrary-length paths
  with checked allocation and propagated write failures, and moves both test
  buffers to checked heap storage. The clean build and all 2,813 generated,
  sample-independent FATE targets then completed with zero compiler warnings,
  errors, or test failures. The combined patch inventory is 126 diagnostics.
- BuildKit's Git version also emitted two non-compiler warnings when a shallow
  clone named the annotated AMF and FFmpeg tags. The shared
  `scripts/ci/checkout-annotated-tag.sh` helper resolves the peeled commit,
  shallow-fetches that exact object, verifies it, and recreates the local tag
  required by version discovery. Docker, dev-container, hosted-integration,
  and smoke consumers all use it; the smoke safety fixture exercises an
  annotated tag and rejects warning/error output.
- On a pristine FFmpeg tree, the first `make -s fate-list` can print a
  `GEN tests/pixfmts.mak` progress line before the target list. Treating every
  output line as a target made the supposedly stronger gate fail for the wrong
  reason. Callers now capture the command status and select only `fate-*`
  entries, while still rejecting an empty list.
- Every maintained FFmpeg build path now uses `--fatal-warnings` and a
  case-insensitive compiler-log gate that also recognizes NVCC's
  `warning #...` form: four image builders, the hosted GCC/Clang/SYCL matrix,
  and the full-series smoke harness. Hosted integration and the smoke harness
  compile all test programs and run every generated, sample-independent FATE
  target inside the same log gate, so production and test translation units
  share the contract. The stock-surface hosted matrix
  applies patch 0019 alone; it remains independent of the fork integration
  series but cannot ignore the pinned source's known warnings. No diagnostic
  suppression is added. The legacy
  `Dockerfile.ffmpeg` path was already broken because it copied a deleted
  one-off patch; it now replays the canonical 19-patch series and omits the
  removed-libnpp no-op like the other builders. The two zip-based builders
  fail immediately on the exact patch that does not apply instead of falling
  through to fuzzy `patch` application.
- The dev image's post-build encoder inventory previously printed `WARN` and
  succeeded when a promised encoder was missing. Encoder enumeration does not
  require a hardware device, so that state is now fatal; the current dev image
  lists all 14 promised software, NVENC, QSV, and AMF encoders.
- The version move itself executes the already-accepted stable-release policy
  in ADR-1240. The warning-clean language-standard selection is separately
  recorded by ADR-1273; the remaining image changes are one-way build defect
  fixes rather than new architecture or release policy.

## Diagnostic inventory

| Observed output | Root cause / classification | Resolution |
| --- | --- | --- |
| Meson: use the built-in language-standard option | Direct `-std`/`/std` project arguments | ADR-1273 preference lists; warning removed |
| `model.c`: comparisons are always false | `xxd` absent, so the built-in table count was compile-time zero | Install `xxd`; make the disabled source path warning-clean and test it |
| `lto-wrapper`: serial compilation of LTRANS jobs | `make` absent from the libvmaf builder | Install `make`; numeric LTO stays parallel without fallback |
| `ldconfig`: `libvmaf.so.3` is not a symbolic link | Plain copy dereferenced the staged SONAME links | Preserve the chain with `cp -a` |
| FFmpeg configure: `libvmaf >= 2.0.0` not found | Handwritten `.pc` advertised product tag `dev` | Copy Meson's interface-versioned `libvmaf.pc` |
| FFmpeg: 70 GCC 14 warnings | Real bounds, fallthrough, truncation, and oversized-stack paths in the pinned stable sources | Patch 0019 fixes the source; rebuilt with VVC enabled and zero warnings |
| FFmpeg FATE: 27 GCC 14 warnings | Oversized automatic buffers in checkasm, APV, and CABAC test code | Move reusable scratch to checked aligned allocations; compile and run FATE under the warning gate |
| FFmpeg: 18 GCC 16 warnings | AAC window bounds, disabled HVQM helpers, and signed SVQ1 recursion were not proved to the newer optimizer | Add fail-closed bounds/configuration invariants; rebuild and run generated FATE targets warning-free |
| FFmpeg full target build: 11 GCC 16 warnings | Tool path truncation/no-return metadata and oversized swresample/snow test stacks | Use checked dynamic paths and heap test buffers; compile every production, tool, example, and test program |
| Git: annotated tag is not a commit | Direct shallow clone asked BuildKit Git to check out the annotated tag object | Resolve and verify the peeled commit, then recreate the local tag with the shared checkout helper |
| `make`: no rule for `GEN tests/pixfmts.mak` | First `fate-list` invocation mixed generated-file progress with target names | Capture successful output, select only `fate-*`, and reject an empty target set |
| `Dockerfile.ffmpeg`: deleted one-off patch input | Compatibility image never migrated to the canonical patch stack | Replay `ffmpeg-patches/series.txt` and fail on the exact unapplied patch |
| CMake `Performing Test ... - Failed` during SVT-AV1 configure | Negative capability probes whose results select supported compiler features; configure and build continue by design | Inspected as probes, not suppressed diagnostics; no repository failure to patch |

## Alternatives explored

| Option | Result | Disposition |
| --- | --- | --- |
| Keep `n9.0.1` | Avoids a patch-release update but defeats ADR-1240's maintained stable-tag policy. | Rejected. |
| Regenerate changed patch text pre-emptively | Adds review noise even though the complete replay is byte-identical. | Rejected. |
| Advance the shared tag and retain identical patches | Exercises the existing automated policy and preserves integration history exactly. | Chosen; only-one-way maintenance update under ADR-1240. |
| Leave pre-existing image warnings/failures for a later PR | Keeps the version diff smaller but leaves the only end-to-end proof broken and contradicts the warning-clean rule. | Rejected; each observed root cause is fixed here with regression coverage. |
| Disable VVC or lower optimization to hide diagnostics | Shrinks the warning set without repairing the affected code and removes a shipped decoder. | Rejected; VVC remains enabled and the same optimized build is warning-clean. |
| Add compiler suppressions | Makes the log quiet while retaining unproved bounds and oversized stack frames. | Rejected; no suppression was added. |

## Open questions

- Hosted Windows, macOS, and GPU lanes remain the final cross-platform proof
  after this draft leaves draft state; local replay and container integration
  cannot substitute for those runners.
- External-sample FATE conformance remains hosted/sample-corpus dependent. The
  local and hosted no-corpus gate compiles all test programs and runs the full
  target list produced by `make -s fate-list` with `SAMPLES` unset.

## Related

- [FFmpeg patch automation](../development/ffmpeg-patch-automation.md)
- [FFmpeg integration patch series](../../ffmpeg-patches/README.md)
- [ADR-0118](../adr/0118-ffmpeg-patch-series-application.md)
- [ADR-1273](../adr/1273-warning-clean-meson-language-standards.md)
