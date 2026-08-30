- **ADR-0700 + ADR-0709 + ADR-0726 cleanup bundle (#321 + #322 + #324 + #334):**
  - (#321) IDE and lint config files updated for the ADR-0700 `libvmaf/` → `core/` rename
    (`.vscode/c_cpp_properties.json`, `.zed/settings.json`, `.github/CODEOWNERS`,
    `.clang-tidy`, `.dockerignore`, `.gitignore`).
  - (#322) Dockerfiles and dev containers updated for the ADR-0700 rename
    (`docker/Dockerfile.production`, `docker/Dockerfile.production-gpu`,
    `docker/dev/*.Dockerfile`, `dev/Containerfile`).
  - (#324) Residual `float_ansnr` references stripped from docs and `ai/data/` after
    ADR-0709 removed the `float_ansnr` extractor.
  - (#334) Vulkan stripped from all user-facing surfaces (CLI help, MCP tools,
    vmaf-tune backend lists, README, mkdocs nav) per ADR-0726.
