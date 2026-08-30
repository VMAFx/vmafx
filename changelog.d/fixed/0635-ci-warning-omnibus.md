- **CI (Windows)**: Replace `ilammy/msvc-dev-cmd@v1.13.0` (Node.js 20,
  deprecated 2026-06-02) with `TheMrMilchmann/setup-msvc-dev@v4.0.0`
  (Node.js 24) on the Windows MSVC + CUDA and MSVC + SYCL build legs.
  (ADR-0635)
- **CI (Windows)**: Pin `runs-on: windows-latest` to `runs-on: windows-2025`
  on all Windows jobs before the 2026-06-15 GitHub redirect to
  `windows-2025-vs2026`. (ADR-0635)
- **CI (macOS)**: Demote the MoltenVK `vulkaninfo` annotation from
  `::warning::` to `::debug::` on hosted runners that lack bare-metal GPU
  access; redirect vulkaninfo stderr to suppress spurious GPU-capability
  messages. (ADR-0635)
- **CI (cache)**: Bump ccache key prefix to `ccache-v2-` to discard stale
  `actions/cache@v4`-format entries that caused `Cache entry deserialization
  failed` warnings after the v4→v5 upgrade. (ADR-0635)
- **Docs**: Fix `docs/mcp/tools.md#run_benchmark` anchor failure under
  `mkdocs build --strict` by removing backticks from the heading and dropping
  the no-longer-needed `<a id>` tag. (ADR-0635)
