<!-- markdownlint-disable MD013 -->
# PSNR (Peak Signal-to-Noise Ratio)

PSNR is a standard full-reference objective video quality metric computing the
ratio between the maximum possible power of a signal and the power of corrupting
noise that affects the fidelity of its representation.

## Mathematical Definition

For a reference frame $R$ and distorted frame $D$ with dimensions $W \times H$ and bit depth $b$,
the Mean Squared Error (MSE) for plane $p$ is defined as:

$$\mathrm{MSE}_p = \frac{1}{W_p \cdot H_p} \sum_{x=0}^{W_p-1} \sum_{y=0}^{H_p-1} \left( R_p(x, y) - D_p(x, y) \right)^2$$

Given the peak signal value $\mathrm{peak}_b = 2^b - 1$ (or 255 scaled for HBD when `reduced_hbd_peak=true`),
the standard PSNR in decibels (dB) is:

$$\mathrm{PSNR}_p = 10 \cdot \log_{10}\left(\frac{\mathrm{peak}_b^2}{\mathrm{MSE}_p}\right)$$

When $R$ and $D$ are byte-identical ($\mathrm{SSE} = 0$, $\mathrm{MSE} = 0$), the mathematical PSNR is infinite.

## Cap Semantics and Saturation Table

By default, libvmaf caps per-frame PSNR scores at $(6 \times bpc) + 12$ dB to avoid infinite values
and provide stable numeric output across pipelines:

| Bit depth | Nominal Peak | PSNR Cap (`psnr_max`) |
|:---------:|:------------:|:---------------------:|
| 8-bit     | 255.0        | 60.0 dB               |
| 10-bit    | 1023.0       | 72.0 dB               |
| 12-bit    | 4095.0       | 84.0 dB               |
| 16-bit    | 65535.0      | 108.0 dB              |

### Truncation vs Uncapped Reporting

In default mode (`uncapped=false`), any genuinely computed finite PSNR exceeding the per-bitdepth
cap is clamped to `psnr_max`. For example, on a 576×324 8-bit frame where exactly one luma sample
differs by +1 ($\mathrm{SSE}=1$), the true PSNR is:

$$10 \cdot \log_{10}(255^2 \times 186624) \approx 100.840479\text{ dB}$$

Under default capped mode, this value is truncated to `60.000000 dB`.

Setting `uncapped=true` reports the true mathematical PSNR (e.g. `100.840479 dB`), while retaining
the `psnr_max` sentinel for byte-identical frames ($\mathrm{SSE}=0$).

## Options

The PSNR feature extractor supports the following options:

| Option | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `uncapped` | bool | `false` | Disable per-bitdepth PSNR capping. When `true`, real PSNR values above 60/72/84/108 dB are reported instead of truncated. Byte-identical frames ($\mathrm{SSE}=0$) remain at `psnr_max`. |
| `min_sse` | double | `0.0` | Constrain the minimum possible SSE, which scales `psnr_max = ceil(10 * log10(peak^2 / (min_sse / (pw * ph))))`. Documented in `features.md` and `cli.md`. |
| `enable_chroma` | bool | `true` | Enable calculation for chroma channels (`psnr_cb`, `psnr_cr`). Set `false` for luma-only. |
| `enable_mse` | bool | `false` | Emit MSE values (`mse_y`, `mse_cb`, `mse_cr`) alongside PSNR. |
| `enable_apsnr` | bool | `false` | Compute aggregate PSNR (`apsnr_y`, `apsnr_cb`, `apsnr_cr`) across the entire video sequence, emitted at flush. |
| `reduced_hbd_peak` | bool | `false` | Reduce HBD peak value to align with scaled 8-bit content ($255 \times 2^{b-8}$). |

> [!NOTE]
> `min_sse` was historically considered as a workaround for the 60 dB truncation (e.g. `--feature psnr=min_sse=0.000001`),
> but setting an artificial minimum SSE distorts the scale factor for finite values. `uncapped=true` is the
> correct, mathematically exact approach to obtain untruncated PSNR without distorting the calculation.

## Usage Examples

### CLI

Enable uncapped reporting for integer PSNR:

```bash
vmaf --reference ref.yuv --distorted dis.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --feature psnr=uncapped=true
```

Enable uncapped reporting for float PSNR:

```bash
vmaf --reference ref.yuv --distorted dis.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --feature float_psnr=uncapped=true
```

## Backends

The `uncapped` option is supported across all backends:

- **CPU**: Scalar, AVX2, AVX-512, NEON (`integer_psnr.c`, `float_psnr.c`)
- **CUDA**: `integer_psnr_cuda.c`, `float_psnr_cuda.c`
- **SYCL**: `integer_psnr_sycl.cpp`, `float_psnr_sycl.cpp`
- **HIP**: `integer_psnr_hip.c`, `float_psnr_hip.c`
- **Metal**: `integer_psnr_metal.mm`, `float_psnr_metal.mm`
