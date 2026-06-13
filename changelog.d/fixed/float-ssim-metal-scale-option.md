**float_ssim Metal: add missing `scale` option** — `float_ssim_metal` silently
dropped the `scale` parameter present on the CPU, HIP, and Vulkan twins.
The option is now registered and validated: v1 enforces `scale=1` (auto-detect
rejects scale > 1 with `-EINVAL` at init), matching the behaviour of
`float_ssim_hip` (ADR-0274) and `ssim_vulkan` (ADR-0189).
