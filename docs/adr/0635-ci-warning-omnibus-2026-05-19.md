<!-- markdownlint-disable MD013 MD033 MD038 MD060 -->
# ADR-0635 — CI Warning Omnibus (2026-05-19)

**Status**: Accepted
**Date**: 2026-05-19
**Deciders**: lusoris
**PR**: chore/ci-warning-omnibus

---

## Context

CI run 26111553607 (workflow_dispatch on master, 2026-05-19) surfaced five
warnings and notices that will become hard failures or day-zero breakage before
the next scheduled merge window:

1. **Node.js 20 deprecation** — `ilammy/msvc-dev-cmd@v1.13.0` (pinned SHA
   `0b201ec74fa43914dc39ae48a89fd1d8cb592756`) uses the Node.js 20 runtime.
   GitHub Actions will force Node.js 24 by default starting 2026-06-02 and
   remove Node.js 20 from runners on 2026-09-16. Surfaced on both Windows GPU
   build legs (MSVC + CUDA and MSVC + oneAPI SYCL).

2. **`windows-latest` redirect** — GitHub will redirect `windows-latest` to
   `windows-2025-vs2026` on 2026-06-15. All three Windows build legs
   (`windows`, `windows-gpu-build`) use `runs-on: windows-latest`.

3. **MoltenVK `vulkaninfo` warning** — the advisory macOS Vulkan lane emits
   `::warning::vulkaninfo did not report MoltenVK/Apple GPU` because hosted
   macOS runners do not expose bare-metal GPU access, so vulkaninfo cannot
   enumerate the Apple GPU even though the MoltenVK ICD loads correctly and
   the build+smoke-test pass. This warning surfaces as a CI annotation on
   every run.

4. **`Cache entry deserialization failed`** — the macOS Vulkan lane's ccache
   restore step logs `WARNING: Cache entry deserialization failed, entry
   ignored`. Root cause: cached entries created by `actions/cache@v4` cannot
   be deserialized by `actions/cache@v5` (format changed). Old entries expire
   naturally over 7 days but produce noisy warnings until then. Bumping the
   cache-key version prefix forces clean entries.

5. **`docs/mcp/tools.md#run_benchmark` anchor failure** — `mkdocs build
   --strict` fails because the heading `## \`run_benchmark\`` generates an
   anchor that mkdocs-material's strict link validator cannot match against
   the cross-link in `docs/mcp/index.md`. PR #1431 added an HTML `<a id>`
   tag as a workaround, but mkdocs-material's validator only sees
   heading-generated slugs, not raw `<a id>` tags. The real fix is to drop
   the backticks from the heading and drop the `<a id>` tag.

## Decision

### Fix 1 — Replace `ilammy/msvc-dev-cmd` with `TheMrMilchmann/setup-msvc-dev@v4`

`ilammy/msvc-dev-cmd` has not shipped a Node.js 24 release as of 2026-05-19;
the latest tag remains v1.13.0 (Node.js 20). `TheMrMilchmann/setup-msvc-dev`
v4.0.0 (released 2025-09-01) uses `node24` as its runtime and provides the
same functional contract: runs `vcvarsall.bat` and exports PATH/INCLUDE/LIB
for cl.exe. No input parameters are required for the default amd64 toolchain.
Pinned to SHA `79dac248aac9d0059f86eae9d8b5bfab4e95e97c` per
`helpers:pinGitHubActionDigests` in `renovate.json`.

### Fix 2 — Pin `runs-on` to `windows-2025`

Changed `runs-on: windows-latest` to `runs-on: windows-2025` on both the
`windows` (MinGW64) and `windows-gpu-build` (MSVC + CUDA/SYCL) jobs. The
`windows-2025` label is the current stable Windows Server 2025 image
(equivalent to the current `windows-latest` — the redirect target is
`windows-2025-vs2026` which carries VS2026 IDE tooling we do not need). This
change is a no-op today and prevents the day-zero redirect surprise on
2026-06-15.

### Fix 3 — Demote MoltenVK vulkaninfo annotation to `::debug::`

On hosted macOS runners, `vulkaninfo` writes GPU-capability warnings to stderr
even when MoltenVK enumeration succeeds. Redirecting stderr to `/dev/null`
(`2>/dev/null`) prevents those lines from surfacing as CI annotations. The
grep on stdout remains the actual correctness gate; when it fails, the message
is emitted at `::debug::` level (visible only with `ACTIONS_RUNNER_DEBUG=true`)
with a note that this is expected on hosted runners without bare-metal GPU
access. Because the lane is already `continue-on-error: true`, no correctness
signal is lost.

### Fix 4 — Bump ccache key version prefix to `ccache-v2-`

Changing the cache-key prefix from `ccache-` to `ccache-v2-` forces all
consumers to start a cold cache, discarding the stale v4-format entries that
caused the deserialization warning. The 7-day warm-up cost is acceptable given
the deserialization warning surfaces on every macOS Vulkan run.

### Fix 5 — Remove backticks and `<a id>` from `## run_benchmark` heading

The heading `## \`run_benchmark\`` causes mkdocs-material strict mode to
reject the cross-link `tools.md#run_benchmark` in `index.md`. The root cause
is that the strict validator resolves anchors from heading slugs, not from
raw HTML `<a id>` tags. Renaming to `## run_benchmark` (no backticks)
produces slug `run_benchmark`, which matches the existing cross-link without
any change to `index.md`. The `<a id>` tag and its `markdownlint-disable`
comment are removed as they are no longer needed.

## Alternatives considered

| Alternative | Reason rejected |
|---|---|
| Bump `ilammy/msvc-dev-cmd` SHA to HEAD | HEAD (`460a772e4cf7358f9f2f23773240813e40e7a894`) is still Node.js 20 — the action author has not released a Node.js 24 build |
| Set `FORCE_JAVASCRIPT_ACTIONS_TO_NODE24=true` | Treats the symptom; the action may behave unexpectedly under a forced runtime it was not tested against |
| Use `microsoft/setup-msbuild@v2` | Exposes MSBuild, not the full MSVC dev command prompt; `cl.exe` would not be on PATH |
| Pin to `windows-2025-vs2026` | Ships VS2026 IDE tools not used by the build; slightly heavier image with no benefit |
| Keep `windows-latest` | Day-zero breakage on 2026-06-15 when the redirect targets a different image |
| Keep `::warning::` on vulkaninfo | Produces a CI annotation on every master push, training maintainers to ignore annotations |
| Suppress the deserialization warning by clearing the cache | Requires a manual repo-settings operation; key-prefix bump is repeatable and reviewable in code |
| Wrap `run_benchmark` heading in a custom anchor directive | mkdocs-material anchor directives are a paid-tier feature; not available on the fork's plan |
| Add backtick-heading support to mkdocs config | There is no documented option to disable slug normalization |

## Consequences

- The Windows GPU build legs no longer emit Node.js 20 deprecation annotations.
- Windows builds are pinned to a stable image label that will not silently
  change on 2026-06-15.
- The macOS Vulkan lane no longer emits spurious warning annotations.
- The ccache deserialization warning disappears after the first warm-up run.
- `mkdocs build --strict` passes for the `run_benchmark` anchor.
- All ccache entries must rebuild from cold on the first run after this change
  (one-time wall-clock hit, ~3–5 min per affected cell).

## References

- req: "fix all CI warnings in one draft PR before they become hard failures"
- Run 26111553607: libvmaf Build Matrix workflow_dispatch on master, 2026-05-19
- `TheMrMilchmann/setup-msvc-dev` releases: v4.0.0 released 2025-09-01, Node.js 24
- `ilammy/msvc-dev-cmd` latest: v1.13.0 (Node.js 20, no Node.js 24 release)
- GitHub Actions runner images README: `windows-2025` = current `windows-latest` image
- ADR-0121: Windows GPU build-only legs original decision
- ADR-0338: macOS Vulkan via MoltenVK advisory lane
