# Research-2043: CAMBI production lint without numerical changes

The complete CPU-profile Cppcheck receipt at `fa792ba4d` covers 277 tracked
sources and 1,149 compile commands. Its `cambi.c` and `cambi_internal.h` blobs
are identical to the starting `07d536aeb` tree. CAMBI has five const/shadow
findings and ten `unusedFunction` findings on private GPU helper exports.
Fresh focused analysis reproduces those findings; clang-tidy starts at zero.

The five source corrections make two validation views read-only, rename the
unused row-callback argument that shadows the reciprocal table, qualify the
private preprocessing input, and qualify the scale-score input in both its
wrapper declaration and definition. No calculation, constant, loop, branch,
return, option, feature name or callback type changes. The shared extractor
callback still requires mutable `VmafPicture *` arguments. Two exact
`constParameterCallback` annotations document that type constraint after
const propagation exposes it; changing the callback type is outside this
internal cleanup.

## Private helpers remain available

All ten trampolines stay in place under the existing ADR-0205 / ADR-1146
private-helper contract. Each receives only an `unusedFunction` annotation,
with the reason cited beside the definition. This preserves the explicitly
requested scaffolds and does not disable unused-function checking elsewhere.
Seven functions have tracked GPU callers outside the CPU build profile:

| Helper suffix after `vmaf_cambi_` | Concrete callers under `core/src/feature/` |
| --- | --- |
| `calculate_c_values` | `cuda/integer_cambi_cuda.c:1033`, `hip/integer_cambi_hip.c:821`, `sycl/integer_cambi_sycl.cpp:870`, `metal/integer_cambi_metal.mm:753` |
| `spatial_pooling` | CUDA:1040, HIP:828, SYCL:877, Metal:759 in the same files |
| `weight_scores_per_scale` | CUDA:1050, HIP:833, SYCL:882, Metal:769 in the same files |
| `get_pixels_in_window` | CUDA:1049, HIP:832, SYCL:881, Metal:768 in the same files |
| `default_callbacks` | CUDA:656, HIP:665, SYCL:699, Metal:564 in the same files |
| `preprocessing` | CUDA:812, HIP:720, SYCL:753, Metal:662 in the same files |
| `init_tvi_and_vlt` | `sycl/integer_cambi_sycl.cpp:653` |

`get_spatial_mask`, `decimate` and `filter_mode` have no tracked callers in
this tree. They remain deliberate private helper scaffolds declared in
`cambi_internal.h` and protected by the package's existing GPU-helper
invariant. An absent CPU caller is not a reason to remove that interface.

The read-only scale-score declaration is consumed directly by all four GPU
wrappers above, with no matching callback-pointer consumer. Their mutable
arrays remain valid arguments to the const-qualified declaration. The
`VmafCambiRangeUpdater`, `VmafCambiDerivativeCalculator` and shared extractor
callback types are unchanged.

## Validation and scope

A fresh GCC 15.2 release build keeps default LTO and assembly enabled, with
CUDA/SYCL/HIP/Metal/DNN/MCP disabled. `meson test -C build test_cambi` passes
all 23 cases before and after. Every existing test assertion remains intact.
The compiled test includes the real private implementation: its entire
263,640-byte `.text` and 45,431-byte `.rodata` sections are byte-identical
before and after. A token comparison independently permits exactly the five
reviewed source edits and the paired header qualification, ignoring only
comments and whitespace; all remaining 11,820 production tokens match.

Actual scoped clang-tidy measures zero warnings, uncited exceptions and
compiler failures. The scoped baseline writer verifies the existing zero
allowance without changing the baseline. Focused Cppcheck has no finding in
either touched native file; an unused inline helper in untouched
`feature_collector.h` still fails that reduced-context invocation, showing
unused checks remain active outside the exact annotations. The complete CPU
profile also reports no warning, style or error in either touched native file;
CAMBI retains only the standard branch-analysis-limit information. Its overall
exit remains 1 on findings elsewhere in the tree. This is touched-file
acceptance, not a whole-tree lint pass.

No new API, user-visible behavior or architectural decision is introduced,
so no new ADR, usage guide or FFmpeg patch refresh is needed. This does not
establish GPU compilation/runtime, Windows, Darwin or Netflix Python golden
acceptance. Commands, exact tool/source hashes, compile variants, analyzer
logs, equivalence script and test receipts are retained under
`.workingdir2/evidence/2026-09-08-cambi-production-lint/`.
