# Research: vmaf-tune codec runtime variants

Companion to [ADR-0644](../adr/0644-vmaf-tune-codec-runtime-variants.md).

## Question

Can `vmaf-tune compare` represent mainline SVT-AV1 and
`juliobbv-p/svt-av1-hdr` in the same report without adding a fake codec adapter?

## Findings

### SVT-AV1-HDR is a runtime/library variant

The SVT-AV1-HDR repository describes itself as SVT-AV1 with perceptual SDR/HDR
enhancements and points users at community FFmpeg builds. The README examples
still encode through FFmpeg's `libsvtav1` wrapper, not a separate encoder name.
The bundled `ffmpeg_plugin` docs likewise describe backports and examples around
`--enable-libsvtav1`, `-c:v libsvtav1`, and `-svtav1-params`.

That means the vmaf-tune adapter key should remain `libsvtav1`. The thing that
changes is the FFmpeg binary and linked SVT library behind that adapter.

### Existing compare shape could not express this

Before this PR, `vmaf-tune compare` had one global `--ffmpeg-bin`. A run could
select a mainline-linked FFmpeg or an HDR-linked FFmpeg, but not both. If an
operator changed `--ffmpeg-bin` to the HDR build, every `libsvtav1` row changed
meaning while still being labelled `libsvtav1`.

### Adapter duplication would be misleading

A `libsvtav1-hdr` adapter would have the same FFmpeg argv shape as the existing
adapter (`-c:v libsvtav1`, same CRF/preset mapping). The only new information
would be runtime identity. Keeping that identity in the compare runtime layer
preserves the adapter invariant from `tools/vmaf-tune/AGENTS.md`: adapters
describe argv shape; compare predicates select runtime binaries.

## Implementation shape

- `ADAPTER@VARIANT` is parsed as a display token.
- The base `ADAPTER` routes through the normal adapter registry, probe, and
  bisect/CRF-sweep encode path.
- `--encoder-ffmpeg-bin TOKEN=PATH` binds a token-local FFmpeg binary.
- Compare rows expose `codec` (display token), `adapter`, `runtime_variant`, and
  `ffmpeg_bin` so downstream encoder-profile consumers can audit provenance.

## References

- SVT-AV1-HDR repository: <https://github.com/juliobbv-p/svt-av1-hdr/>
- SVT-AV1-HDR FFmpeg plugin README: <https://raw.githubusercontent.com/juliobbv-p/svt-av1-hdr/main/ffmpeg_plugin/README.md>
- [ADR-0641](../adr/0641-dev-container-encoder-probe-hardening.md)
- [ADR-0644](../adr/0644-vmaf-tune-codec-runtime-variants.md)
