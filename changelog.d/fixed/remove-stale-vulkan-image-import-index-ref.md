- docs: remove stale "Vulkan image import" entry from `docs/index.md` C API section.
  The Vulkan backend was dropped in ADR-0726 (2026-05-28); the list item falsely
  implied `api/vulkan-image-import.md` described a live API. The page itself already
  carried its ADR-0726 removal banner (added separately); the nav entry in
  `mkdocs.yml` was already removed. This PR removes the lone surviving stale pointer
  in `docs/index.md`.
