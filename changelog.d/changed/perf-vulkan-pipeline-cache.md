Add disk-persistent `VkPipelineCache` to the Vulkan backend. The cache is
keyed by physical device UUID and stored under
`${XDG_CACHE_HOME:-$HOME/.cache}/vmaf/vulkan/`. Warm-start Vulkan runs skip
driver pipeline re-linking, saving an estimated 200–700 ms per full
multi-feature run. First cold run is unchanged; cache mismatches (driver
upgrade) are silently invalidated by the driver. (ADR-0470, VK-4)
