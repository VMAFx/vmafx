- **Dev container: the Vulkan loader and lavapipe are in the image.** The
  dev image installs the Vulkan loader, its headers, Mesa's Vulkan drivers
  (with lavapipe, the CPU Vulkan driver) and `vulkaninfo`, and its build fails
  when the loader lists no lavapipe. Vulkan-dependent tests build in every lane
  of the one image and have a Vulkan device without a GPU
  ([dev container](docs/development/dev-mcp.md#vulkan-loader-and-lavapipe),
  ADR-3137).
