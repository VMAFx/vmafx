# ROI reader and placeholder boundary audit (2026-09-08)

## Finding and scope

The configured CPU clang-tidy 22.1.8 run on integration head
`8100a2caa34bdd3810f650fc2591220470704ccf` identified a potentially invalid
shift in `read_luma8` and an allocation-bound diagnostic in
`fill_placeholder_saliency`. Source and sanitizer probes distinguish three
cases:

- **Reproduced valid-input bug:** 10-, 12- and 16-bit maximum samples
  converted to zero. Round-half-up produces 256 at the upper edge; casting
  directly to `uint8_t` wrapped white to black. The corrected conversion
  saturates at 255 after rounding and retains the existing clamp of encoded
  samples to the declared bit-depth range.
- **Private-helper precondition gap:** directly calling the old reader with
  depth 7 triggered UBSan's invalid-shift diagnostic. The public CLI already
  rejects unsupported depths; this is local defensive validation, not a newly
  demonstrated CLI route. Invalid depth, count and null buffer inputs now fail
  before allocation, reading or output writes.
- **Analyzer modeling:** the placeholder trace lost the relation between the
  allocation and nested width/height loops. Valid 1x1, 1x2, 2x1 and 2x3 probes
  did not reproduce an out-of-bounds write. The helper now validates the supplied
  count against the dimensions and traverses exactly that count. Integer row
  and column coordinates preserve the original radial arithmetic.

The existing dimension range is still 1 through 16384 on each axis. Its maximum
plane has 268435456 samples; even the four-byte float allocation fits a 32-bit
`size_t`. No dimension cap, sidecar layout, QP clamp, model API, Netflix scoring
path or golden assertion changes.

## Implementation and compatibility

Private helpers live in `core/tools/vmaf_roi_input.h`, following the existing
`vmaf_roi_core.h` shared-header pattern. The CLI and boundary tests compile the
same implementation without exporting test APIs or including an entire CLI C
translation unit. This is a bug fix within
[ADR-0247](../adr/0247-vmaf-roi-tool.md); no alternatives: local validation and
saturating conversion implement the existing bounded luma8 contract.

The existing [ADR-1138](../adr/1138-c-translation-units-keep-null.md) C `NULL`
compatibility brackets are applied to the touched C units and shared header.
The existing getopt suppression is cited to ADR-0247's single-threaded CLI
parsing invariant. No other warning policy or suppression is added.

## Validation recipe and limits

Use an isolated CPU build with all GPU backends, DNN, MCP and docs disabled,
`--buildtype debug -Db_sanitize=address,undefined`, then:

```bash
ninja -C build tools/vmaf_roi test/test_vmaf_roi test/test_vmaf_roi_bounds
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  meson test -C build --print-errorlogs \
  test_vmaf_roi test_vmaf_roi_bounds test_vmaf_roi_high_bitdepth
```

The boundary test covers all 65536 encoded values at each of 10, 12 and 16 bits,
including monotonicity, saturation and rounding thresholds; 8-bit identity;
short input; invalid depths/counts/buffers; degenerate and odd placeholder
shapes; and output canaries. The existing CLI smoke verifies high-bit-depth
frame seeking and invalid input. No real ONNX model, GPU or encoder was used.

Native validation uses the cached CPU image
`sha256:4e2b0298690d730e5ccbd6bd62d4ba9a03935808d4a88549a84e6bf7562f577d`,
CPU affinity 28–31 and no network/GPU access. The build is private to this
branch. Its actual Meson compile database feeds the configured lint driver's
private analyzer adapter; only GCC numeric LTO spelling is translated, with
original build commands retained. Both touched translation units and the new
shared header have zero clang-tidy diagnostics. The generated CPU ratchet
reduces the ROI translation unit from 22 warnings to zero and its uncited
suppression count from one to zero. Whole-baseline totals fall from 1457 to
1435 warnings and from 59 to 58 uncited suppressions, retaining the previous
full-run metadata and every unmeasured entry.

Receipts are retained under `.workingdir2/evidence/roi-reader-bounds-20260908`
with commands and before/after source hashes. This focused acceptance is not a
claim that whole-tree `make lint`, `make test`, Windows or release gates pass.
