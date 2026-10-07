# Sample range

A picture of bit depth `bpc` must hold samples of at most 2^bpc - 1: 255 at
8 bits, 1023 at 10 bits, 4095 at 12 bits. A 10- or 12-bit picture keeps its
samples in `uint16_t`, so storage alone does not stop a larger value. Such a
picture is invalid input to libvmaf. The default path does not check for it,
and with a sample above the limit the CPU extractors and their GPU twins can
give different scores, because each implementation sizes its integers for
in-range samples
([state row T-OUT-OF-RANGE-SAMPLES-TWIN-DIVERGENCE-2026-10-05](../state.md),
[ADR-1918](../adr/1918-sample-range-contract-opt-in-check.md)).

## Turning the check on

```c
VmafContext *vmaf = NULL;
int err = vmaf_init(&vmaf, cfg);
err = err ? err : vmaf_set_sample_range_check_enabled(vmaf, 1);
/* ... vmaf_use_feature() / vmaf_use_features_from_model() ... */
err = vmaf_read_pictures(vmaf, &ref, &dist, index);
if (err == -EINVAL) {
    /* a sample was out of range (or another argument was invalid); the log
     * names the picture, plane, row, column and value */
}
```

With the check on, every `vmaf_read_pictures()` call scans both pictures before
it extracts anything. The first sample above 2^bpc - 1, in raster order of
plane 0, 1 and 2, makes the call return `-EINVAL`. The pictures are released
as for any other error (see [Pictures](pictures.md)), and the log names it:

```text
libvmaf ERROR vmaf_read_pictures: reference picture, plane 1, row 3, column 7: sample 1500 is above 1023, the largest 10-bit value
```

| Picture | With the check on |
| --- | --- |
| 8 or 16 bits | Read: no sample can be out of range. |
| 9 to 15 bits, every sample at most 2^bpc - 1 | Read. |
| 9 to 15 bits, a sample above 2^bpc - 1 | `-EINVAL`, nothing extracted. |
| In device memory (CUDA, SYCL or HIP picture) | `-ENOTSUP`: the host cannot scan it. |

`vmaf_set_sample_range_check_enabled(vmaf, 0)` turns it off again. Off is the
default, and then `vmaf_read_pictures()` reads no sample for it: the default
path costs one test of a flag per call.

## VMAFx API

On the [VMAFx API](vmafx/index.md#contexts-and-logging) the check is the
context option `check_sample_range`, with the same table:

```c
VmafxStatus status = vmafx_context_set_option(context, "check_sample_range", "1", &error);
/* vmafx_submit(): VMAFX_E_INVALID for a sample above 2^bpc - 1,
 * VMAFX_E_NOTSUP for a frame in device memory */
```

`vmaf_set_sample_range_check_enabled()` sets that option of the context behind
the libvmaf handle.

## Command line

`vmaf --check-sample-range` (underscore alias `--check_sample_range`) turns
the check on for a run; an out-of-range frame stops the run with a non-zero
exit status and the message above. See the [CLI reference](../usage/cli.md#input-flags).

## FFmpeg filters

The `libvmaf` FFmpeg filters do not expose the check. Their frames come from
FFmpeg's decoders and scalers, which write samples inside the declared bit
depth; a caller that feeds raw samples of unknown range scores through the
command line or the C API with the check on.
