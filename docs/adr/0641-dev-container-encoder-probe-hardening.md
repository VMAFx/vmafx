<!-- markdownlint-disable MD013 MD060 -->
# ADR-0641: Harden dev-container encoder probes and compare reports

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: lusoris
- **Tags**: `dev-container`, `vmaf-tune`, `ffmpeg`, `qsv`, `amf`, `reports`, `fork-local`

## Context

The BBB v16 dev-container probe stalled with an incomplete compare report and multiple
misleading encoder failures. The host is a mixed-GPU machine where `/dev/dri/renderD128`
belongs to NVIDIA, `/dev/dri/renderD129` belongs to Intel Arc, and `/dev/dri/renderD130`
belongs to the AMD iGPU. The QSV helper hard-coded `/dev/dri/renderD128`, so FFmpeg tried
to initialise Intel QSV through the NVIDIA render node. Forcing the Intel node progressed
VA-API initialisation but still failed with `Error creating a MFX session: -9` because the
container carried the oneVPL dispatcher (`libvpl.so`) without the Gen GPU runtime
(`libmfx-gen.so`).

The same probe also hid AMF's actionable runtime failure (`libamfrt64.so.1` absent) behind
later muxer noise, reported the `dev-mcp` service as unhealthy because the compose
healthcheck looked for a Unix socket while the entrypoint exposes MCP over stdio, emitted
raw markdown tables where operators expected the profile-card report format, and kept the
old CPU default encoder set (`libx264,libx265,libsvtav1,libvpx-vp9`) that makes long
compare sweeps spend too much wall time on CPU-only codec coverage that is not part of the
current decision.

## Decision

We will make the dev-container compare path self-select the correct Intel render node for
QSV, build the pinned Intel oneVPL GPU runtime into the image under the dispatcher-visible
multiarch libdir, surface actionable hardware
probe error lines, align the compose healthcheck with the stdio MCP entrypoint, let
`vmaf-tune compare --format html|both` emit the profile-card reports directly, reduce the
default CPU compare set to `libx265,libsvtav1`, and treat pre-decoded raw shared-reference
bisects as requiring one distorted decode of disk headroom rather than two raw streams.
BBB v9-era probe recipes are retired as runnable baselines; their artifacts remain useful
only as historical bug evidence. The dev-container FFmpeg rebuild is also the patch-stack
replay gate, so the SYCL FFmpeg integration patch is refreshed to call
`vmaf_sycl_state_free(&s->sycl_state)` against the current public API.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `/dev/dri/renderD128` as the QSV default | Simple; preserves prior CLI default | Wrong on any multi-GPU host where Intel is not renderD128 | The dev host already demonstrates this failure; auto-discovery is deterministic via sysfs vendor IDs |
| Require operators to pass `--vaapi-device /dev/dri/renderD129` | No new helper code | Repeats the same footgun in every command and CI script | The default should be safe on the common mixed-GPU case, while explicit paths still override |
| Bind-mount host `libmfx-gen.so` into the container | Fast local workaround | Host-specific, non-reproducible, and invisible to image CI | The container must be self-contained; build the pinned runtime from source and install it where the dispatcher searches |
| Start a UDS MCP daemon so the old healthcheck passes | Preserves socket healthcheck | Adds another always-on service and diverges from the stdio entrypoint used by operators | Healthcheck should validate the actual container runtime contract |
| Keep markdown-only compare output and require a separate report command | Smaller CLI change | Produces unfinished reports for long probes and loses the profile-card artifact operators expect | `compare` already has all data needed to call the report renderer |
| Keep `libx264` and `libvpx-vp9` in the default compare set | Broader archival coverage per run | Slower BBB sweeps; these codecs remain available explicitly | Default should cover the current production CPU decision set; explicit `--encoders` remains full-featured |

## Consequences

- **Positive**: QSV probes use the Intel render node by default on mixed-GPU hosts, and a
  rebuilt dev container has the runtime library in the path needed to create a oneVPL GPU session.
  AMF/QSV probe failures point at the missing runtime instead of trailing FFmpeg noise.
  The `dev-mcp` service health reflects whether the toolchain is callable. Compare sweeps
  can emit the finished HTML/Markdown profile report in one command.
- **Negative**: The dev image build gains a source build of `intel/vpl-gpu-rt`, making
  rebuilds slower and dependent on the pinned upstream tag staying buildable. The branch
  also touches one FFmpeg patch-stack entry, so `ffmpeg-patches/0003-*` needs to be kept
  in sync during any future FFmpeg tag refresh.
- **Neutral / follow-ups**: SVT-AV1-HDR (`juliobbv-p/svt-av1-hdr`) remains a separate
  runtime-identity gap. The fork ships HDR-focused SVT-AV1 changes with community
  FFmpeg builds, but vmaf-tune cannot yet select a pinned
  SVT-AV1-HDR runtime independently from mainline `libsvtav1`. Comparing mainline
  SVT-AV1 and SVT-AV1-HDR in the same sweep requires runtime-variant dispatch, not a
  fake second encoder token.

## References

- Research digest: [dev-container encoder probe hardening](../research/0641-dev-container-encoder-probe-hardening.md).
- `dev/Containerfile` oneVPL runtime layer.
- `dev/docker-compose.yml` `dev-mcp` healthcheck.
- `tools/vmaf-tune/src/vmaftune/compare.py`, `cli.py`, `bisect.py`, and
  `hw_devices.py`.
- `ffmpeg-patches/0003-libvmaf-wire-sycl-backend-selector.patch`.
- `dev/AGENTS.md` FFmpeg encoder invariants.
- `docs/usage/vmaf-tune.md` compare and QSV operator docs.
- Source: `req` — "fix it properly and fully..."
- Source: `req` — "the host has an arc? so qsv must work"
- Source: `req` — "amf should be working with the agpu?"
- Source: `req` — "the report is not like the finished reports"
- Source: `req` — "then I want you to reduce the cpu only codecs, I guess x265 and av1 on cpu are enough, gpu encoders stay"
- Source: `req` — "and do we have adapters for those 3 av1 codecs?"
- Source: `req` — "nvm, fuck psy, this is the new one [juliobbv-p/svt-av1-hdr](https://github.com/juliobbv-p/svt-av1-hdr/)"
- Source: `req` — "oh and we can deactivate v9 in our bbb runs as well, fully useless"
