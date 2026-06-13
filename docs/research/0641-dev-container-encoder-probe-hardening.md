# Research: Dev-container encoder probe hardening

Companion to [ADR-0641](../adr/0641-dev-container-encoder-probe-hardening.md).

## Findings

### Render-node topology

The dev host is a mixed-GPU machine. `/dev/dri/renderD128` maps to NVIDIA, while
the Intel Arc render node is `/dev/dri/renderD129`. The previous vmaf-tune QSV
default pointed at renderD128, so FFmpeg initialised VA-API against the wrong
vendor before attempting the QSV bridge.

The robust selector is sysfs vendor ID discovery:

- `/sys/class/drm/renderD*/device/vendor == 0x8086` identifies Intel render nodes.
- `/dev/dri/by-path/*-render` gives a stable PCI-sorted view when present.
- Explicit `--vaapi-device PATH` still wins for reproducibility and remote hosts.

### QSV runtime split

The container had FFmpeg compiled with `--enable-libvpl` and the distro
`libvpl-dev` package installed. That is enough to expose `h264_qsv`, `hevc_qsv`,
and `av1_qsv` in `ffmpeg -encoders`, but not enough to create an Arc/iGPU MFX
session. Runtime creation failed with:

```text
Error creating a MFX session: -9.
```

The missing component is Intel's Gen GPU runtime (`libmfx-gen.so`). The host
Arch install carries it via `vpl-gpu-rt 26.1.5`, but the container had no
runtime in the path searched by Ubuntu's oneVPL dispatcher. Installing the
runtime under `/usr/local/lib` was not sufficient by itself: FFmpeg still
failed with `MFX_ERR_NOT_FOUND` unless `ONEVPL_SEARCH_PATH=/usr/local/lib`
was injected. The durable container fix installs the pinned runtime under
`/usr/lib/x86_64-linux-gnu/`, where `libvpl.so.2` searches by default.

Upstream `intel/vpl-gpu-rt` publishes source tags but no release assets for the
latest tag checked during this investigation (`intel-onevpl-26.1.5`). Its README
documents the normal CMake build flow, and the source declares `libva`, `libdrm`,
and `libva-drm` pkg-config requirements. The container fix therefore builds the
pinned source tag and verifies `libmfx-gen.so` exists in the dispatcher-visible
multiarch libdir after install.

### AMF runtime diagnostics

AMF headers are enough for FFmpeg compile-time enablement, but AMF encode
runtime requires AMD's proprietary `libamfrt64.so.1`. The dev host and the
container do not currently provide that library. FFmpeg's stderr contained the
useful `libamfrt64.so.1` line, but the probe reported a later muxer/generic line
instead. The probe error extractor now prefers known actionable hardware-runtime
lines (`libamfrt64`, `AMF_NOT_SUPPORTED`, MFX-session creation, VA-API init,
device access errors) over trailing noise.

### Compose healthcheck mismatch

The `dev-mcp` service entrypoint exposes MCP over stdio:

```shell
docker exec -i vmaf-dev-mcp /opt/vmaf-venv/bin/vmaf-mcp
```

It does not create `/sockets/vmaf-mcp.sock` unless a caller explicitly starts a
UDS transport. The previous compose healthcheck tested for that socket and left
the service permanently `unhealthy`, which in turn blocked dependent probe
services despite a working vmaf binary and stdio MCP runtime. The healthcheck now
uses `vmaf --version`, matching the actual always-on container contract.

### Report artifact gap

The v16 compare probe produced raw markdown tables instead of the profile-card
HTML/Markdown report format used by finished BBB reports. The compare command
already has the source metadata and compare/sweep rows needed to render
`ReportData`, so a separate manual `vmaf-tune report` call is unnecessary for
the common long-probe path. `compare --format html|both --output PATH` now emits
the finished report artifacts directly after the bisect/sweep completes.

### Disk-headroom estimate

After the shared-reference decode fix, `_run_compare` decodes the reference once
and passes a raw `.yuv` path to every bisect worker. Mid-run disk checks inside
the worker should therefore reserve room for the distorted leg only. Retaining
the old 2× source-size estimate incorrectly rejects valid runs on large BBB
sources. Container sources still use the conservative 2× estimate because both
legs may need decode scratch.

### BBB v9 retirement

The BBB v9 reports were useful as bug evidence, but they are no longer a valid
run recipe. They predate the shared-reference decode fix, the QSV auto-device
selection, the direct profile-report output path, and the narrowed production CPU
encoder set. Current BBB probes should be labelled from the latest run revision
and use `compare --format both`; v9 artifacts remain archival references only.

### FFmpeg SYCL patch replay

Rebuilding the dev image replays the in-tree `ffmpeg-patches/` stack against
FFmpeg n8.1.1 before compiling the container FFmpeg. That surfaced a stale SYCL
cleanup call in `0003-libvmaf-wire-sycl-backend-selector.patch`: the current
public header declares `vmaf_sycl_state_free(VmafSyclState **sycl_state)`, while
the patch still passed `s->sycl_state`. HIP, Vulkan, and Metal already pass a
double pointer; CUDA remains a single pointer per `libvmaf_cuda.h`. The SYCL
patch is refreshed to match the public API and let the container FFmpeg build
act as the full patch-stack replay gate.

## Remaining gaps

- SVT-AV1-HDR (`juliobbv-p/svt-av1-hdr`) is not independently selectable today.
  GitHub metadata checked 2026-05-20 shows it is a BSD-3-Clause-Clear fork of
  `psy-ex/svt-av1-psy`; the README describes HDR-focused SVT-AV1 perceptual
  changes and points to community FFmpeg builds. The adapter follow-up must verify
  whether those builds expose a distinct encoder name or the existing `libsvtav1`
  FFmpeg wrapper. If it is the same wrapper, comparing mainline SVT-AV1 and
  SVT-AV1-HDR in one sweep requires per-codec FFmpeg binary dispatch or an
  explicit runtime-variant abstraction.
- The profile-card report renderer deserves its own graph/layout audit. The new
  direct `compare --format both` path makes report quality more visible, but it
  does not redesign chart density, labels, scales, or artifact packaging.

## Verification targets

- QSV device resolver unit tests cover Intel sysfs discovery, explicit-path
  preservation, and fallback when no Intel node is present.
- Compare probe tests cover actionable hardware-runtime stderr extraction.
- Compare CLI tests cover direct `--format both` profile-report emission.
- Bisect disk tests cover 2× container headroom and 1.1× raw shared-reference
  headroom.
- Container validation target: rebuild `dev-mcp`, verify `libmfx-gen.so` exists
  under `/usr/lib/x86_64-linux-gnu/`, `ffmpeg -encoders` exposes QSV encoders,
  and a 1-frame QSV dummy encode can create a session on the Intel render node
  without setting `ONEVPL_SEARCH_PATH`.
- FFmpeg patch-stack validation target: the dev-container FFmpeg build must compile
  the cumulative n8.1.1 patch stack, including the SYCL state-free call in patch
  `0003`.
