<!-- markdownlint-disable MD013 MD038 MD060 -->
# Research: CI Warning Omnibus (2026-05-19)

**Context**: CI run 26111553607 (workflow_dispatch on master) surfaced five
warnings / notices that require pre-emptive fixes before the 2026-06-02 /
2026-06-15 hard-failure dates. Companion to ADR-0635.

---

## Warning 1 — Node.js 20 deprecation on `ilammy/msvc-dev-cmd`

### Investigation

`ilammy/msvc-dev-cmd` is the community-standard action for activating the MSVC
developer command prompt on Windows GitHub Actions runners. The pinned SHA
`0b201ec74fa43914dc39ae48a89fd1d8cb592756` is v1.13.0, the latest published
release as of 2026-05-19. Verified via:

```text
gh api repos/ilammy/msvc-dev-cmd/releases --jq '.[].tag_name'
# → v1.13.0 (only release)
gh api repos/ilammy/msvc-dev-cmd/commits/HEAD --jq '.sha'
# → 460a772e4cf7358f9f2f23773240813e40e7a894 (no new release past v1.13.0)
```

The action's `package.json` pins `@actions/core` to a version compiled for
Node.js 20. The action author has not published a v2 or a Node.js 24 build.

### Candidate replacements

| Action | Node version | Notes |
|---|---|---|
| `TheMrMilchmann/setup-msvc-dev@v4.0.0` | node24 | Functionally identical to ilammy; released 2025-09-01; inputs: `arch` (default amd64), `vs-path`, `sdk`, `toolset` |
| `microsoft/setup-msbuild@v2` | node20 | Exposes MSBuild entry point only — `cl.exe` not on PATH |
| `microsoft/setup-msbuild@v3` (hypothetical) | — | Does not exist as of 2026-05-19 |
| Force `FORCE_JAVASCRIPT_ACTIONS_TO_NODE24=true` | — | Treats symptom; action untested under forced Node.js 24 |

**Selected**: `TheMrMilchmann/setup-msvc-dev@v4.0.0`.

Action YAML verified:

```yaml
runs:
  using: 'node24'
  main: 'dist/index.cjs'
```

No required inputs; `arch` defaults to amd64 — matching the current ilammy
invocation (no inputs specified). SHA: `79dac248aac9d0059f86eae9d8b5bfab4e95e97c`.

---

## Warning 2 — `windows-latest` runner redirect

### Investigation

GitHub announced that `windows-latest` will redirect to `windows-2025-vs2026`
on 2026-06-15. The fork's Windows jobs (`windows`, `windows-gpu-build`) both
use `runs-on: windows-latest`.

Runner image matrix (from `actions/runner-images` README, live 2026-05-19):

| Label | Image |
|---|---|
| `windows-latest` or `windows-2025` | Windows Server 2025 |
| `windows-2025-vs2026` | Windows Server 2025 + VS2026 |

The redirect target is `windows-2025-vs2026` (VS2026), not `windows-2025`.
However, the fork's MSVC builds use `cl.exe` from the toolset, not the VS IDE.
`windows-2025` is the minimal stable label that is equivalent to the current
`windows-latest` and will not pick up the VS2026 toolset delta.

**Selected**: `windows-2025`.

Strawberry ccache availability (`C:\Strawberry\c\bin\ccache.exe`) confirmed
present on `windows-2025` — same base image as current `windows-latest`.

---

## Warning 3 — MoltenVK `vulkaninfo` warning annotation

### Investigation

The `Install MoltenVK + Vulkan loader/headers (macOS)` step runs:

```bash
vulkaninfo --summary 2>&1 | tee /tmp/vulkaninfo.txt || true
if ! grep -qi 'MoltenVK\|Apple' /tmp/vulkaninfo.txt; then
  echo "::warning::vulkaninfo did not report MoltenVK/Apple GPU — see /tmp/vulkaninfo.txt"
fi
```

On hosted `macos-latest` runners (Apple M-series virtualized), `vulkaninfo`
writes GPU-capability warnings to stderr (e.g. "No display server running —
GPU enumeration incomplete") that are captured by `2>&1` and surfaced as CI
annotations regardless of whether the MoltenVK ICD loads. The grep on stdout
is the correctness gate; the `::warning::` fires when stdout does not contain
"MoltenVK" or "Apple", which happens when stderr dominates the tee output.

Confirmed: the MoltenVK lane is `continue-on-error: true` (ADR-0338), so the
warning does not block CI. But it produces a persistent annotation on every
master push, habituating maintainers to ignore the annotation panel.

**Fix**: redirect stderr to `/dev/null` so only stdout is teed; demote the
fallback message from `::warning::` to `::debug::` with an explanatory note.

---

## Warning 4 — `Cache entry deserialization failed`

### Investigation

The macOS Vulkan lane's ccache restore step logs:

```text
WARNING: Cache entry deserialization failed, entry ignored
```

The ccache key is `ccache-${{ matrix.os }}-${{ matrix.CC }}-${{ matrix.meson_extra }}-${{ github.sha }}`.
`actions/cache` was upgraded from v4 to v5 (SHA `27d5ce7f107fe9357f9df03efb73ab90386fccae`)
in a prior PR. The v5 action changed the on-disk cache entry format; entries
saved by v4 cannot be deserialized by v5. Old entries expire after 7 days but
produce the deserialization warning on every run until expiry.

**Fix**: bump the key prefix from `ccache-` to `ccache-v2-`, forcing all
consumers to start a fresh cache. One-time ~3–5 min wall-clock hit per cell;
no correctness impact (ccache is a build accelerator, not a correctness gate).

No other cache steps use a `ccache-` prefix — the Vulkan subproject packagecache
(`meson-packagecache-vulkan-...`) and fixture cache (`vmaf-fixtures-resource-v1-...`)
are not affected by the v4→v5 format change (they store plain file archives,
not build-object caches with v4/v5-specific metadata).

---

## Warning 5 — `docs/mcp/tools.md#run_benchmark` anchor failure

### Investigation

`mkdocs build --strict` fails with a broken anchor warning when
`docs/mcp/index.md` links to `tools.md#run_benchmark` and the heading in
`tools.md` is `## \`run_benchmark\``.

Root cause: mkdocs-material's link validator resolves anchors from
heading-generated slugs. The slug generation for `## \`run_benchmark\`` is
implementation-defined — some versions strip backticks and produce
`run_benchmark`; others encode them as `run_benchmark_1` or similar. The
strict validator rejects any slug it cannot confirm.

PR #1431 added `<a id="run_benchmark"></a>` as a workaround, but mkdocs-material
does not index raw HTML `<a id>` tags for link validation — only Markdown
headings contribute to the slug map. The HTML anchor is visible to browsers
but invisible to the strict checker.

Other headings in `tools.md` with the same backtick pattern (`## \`vmaf_score\``,
`## \`list_models\``, etc.) are not cross-linked from `index.md` or use
fragment-free links, so they are not caught by the strict validator.

**Fix**: rename the heading to `## run_benchmark` (no backticks) and drop the
`<a id>` tag and its `markdownlint-disable-next-line MD033` comment. The slug
`run_benchmark` is stable and matches the existing cross-link in `index.md`
without any further changes.
