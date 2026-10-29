<!-- markdownlint-disable MD013 MD060 -->
# Research-3388: where VMAFx listed its encoders, how far the lists drifted, and what is still missing

- **Status**: Active
- **Workstream**: [ADR-3388](../adr/3388-central-encoder-registry.md)
- **Last updated**: 2026-10-29

Before [ADR-3388](../adr/3388-central-encoder-registry.md) the same encoder
facts were written by hand in four places. This digest records what each list
held at `51e6facdd` (master on 2026-10-29), the drift the registry exposed, and
how the replacement was checked. The user page is
[encoder-registry.md](../usage/encoder-registry.md).

## 1. The four lists

| List | File | Held |
|---|---|---|
| Python adapters | `tools/vmaf-tune/src/vmaftune/codec_adapters/` (`_REGISTRY`) | 19 adapters: knob, window, default, presets, version, probe settings, capability flags |
| Go tuner table | `pkg/encoder/adapter.go` (`adapterRegistry`) | the same 19 ids with windows, defaults, preset vocabularies |
| Go adapter port | `pkg/codecadapter/codecadapter.go` | the same facts again, per constructor |
| Bisect window | `tools/vmaf-tune/src/vmaftune/bisect.py` (`_ABSOLUTE_CRF_RANGE_BY_NAME`) | absolute CRF range for 5 software encoders |

## 2. Drift found

- `AdapterVersion` in `pkg/codecadapter` was empty for `h264_nvenc`,
  `hevc_nvenc`, `av1_nvenc`, `h264_amf`, `hevc_amf`, `av1_amf`, `libaom-av1`
  and `libsvtav1`, while the Python adapters carry `"1"` (NVENC, libaom,
  SVT-AV1) and `"2"` (AMF). `pkg/codecadapter/adapter_version_test.go` pinned
  only the eleven ids that agreed. Once the Go adapters read the registry, the
  test failed on exactly these eight; the pin now lists all nineteen.
- The bisect table agreed with the adapters for its five rows. For the
  fourteen other encoders the bisect fell back to the adapter's quality window;
  the registry's `absolute_range` for those rows equals their window, so the
  bisect window is unchanged for every encoder
  (`tests/test_encoder_registry.py::test_bisect_window_is_registry_absolute_range`).

## 3. Checks run

| Check | Command | Result |
|---|---|---|
| Generated outputs current | `python3 scripts/codegen/vmafx-api.py --check` | 90 generated files match |
| Loader rules, each refusing its planted defect | `python3 -m unittest test_vmafx_api_encoders` (in `scripts/codegen/tests`) | 16 tests OK |
| Python adapters equal rows | `pytest tools/vmaf-tune/tests` (fingerprint extra installed) | 2309 passed, 33 skipped (optional extras), 0 failed |
| Go consumers | `go test ./pkg/encoder ./pkg/encoderregistry ./pkg/codecadapter` | ok |
| Mutation, Python copy | `absolute_range` of `libx264` set to `[0, 50]`, version `"99"` | `test_adapter_matches_registry_row[libx264]`, `test_python_copy_matches_go_copy`, `test_absolute_crf_range_table_lookup` fail |
| Mutation, Go copy | `quality_range` of `libx264` set to `[1, 51]`, version `"99"` | `TestAdapter_QualityWindows`, `TestAdapterVersionMatchesPython`, `TestValidate`, `TestAdapterMetadata_matchesPythonGolden`, `TestRegistryMatchesPythonMetadata` fail |

The second slice adds every encoder VMAFx does not run yet as a gap row, adds the tuning-profile variants, and adds a checker that compares each row with its live upstream (sections 4 to 9).

## 4. What the registry holds now

| Field | Counts |
|---|---|
| Rows | 106 encoders, 8 variants |
| Status | 19 implemented, 69 planned, 10 listed, 8 decision |
| Class | 46 delivery, 7 mezzanine, 3 lossless, 1 low-latency, 3 texture, 8 research, 1 decode-only, 3 commercial-listed, 34 distortion-source |
| Paradigm | 98 classic, 3 ml-assisted, 1 hybrid-neural, 4 neural-e2e |
| Licence class | 20 permissive, 56 copyleft-weak, 5 copyleft-strong, 1 patent-licensed-binary, 7 proprietary-sdk, 17 unknown |
| Interface | 78 ffmpeg, 12 cli, 7 sdk, 9 decode-only |

The eight `decision` rows (`evc-main`, `notchlc`, `bink`, `lcevc-encoder-sdk`, `maxine-sdk`, `commercial-hevc-sdk`, `commercial-avc-sdk`, `commercial-vvc-sdk`) are listed only. Each needs an operator decision before any implementation work, because each is licence-gated or proprietary. Commercial SDK rows carry generic ids on purpose.

Variants added: `libsvtav1@svt-av1-psyex`, `libx265@grain`, `libx265@texture`, `libx264@psy-rd`, `libx264@psy-trellis`, `libx264@mbtree`, `libx264@grain` (tuning profiles), next to the existing `libsvtav1@svt-av1-hdr` runtime fork.

## 5. New fields and their rules

Both fields are enforced in `scripts/codegen/vmafx_api/encoders.py` and tested in `scripts/codegen/tests/test_vmafx_api_encoders.py`.

- `artifact_domains` (`dct-blocking`, `wavelet-blur`, `vq-mosaic`, `lossless-reference`) is set exactly when `class = "distortion-source"`. These rows exist to feed one degradation corpus per artifact domain, not to be tuned.
- `awaits_ffmpeg = true` marks an [ADR-0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md)-style placeholder: the row names an FFmpeg encoder that FFmpeg master does not ship yet. It needs an `ffmpeg_encoder`. Today only `av1_videotoolbox` carries it.

## 6. How each distortion-source domain was verified

Each domain comes from the codec's technical description, mostly the MultimediaWiki (`wiki.multimedia.cx`) codec pages:

| Domain | Rows (examples) | Basis |
|---|---|---|
| `vq-mosaic` | Indeo 3, Cinepak, Apple RPZA, MS Video 1, SVQ1 (5 rows) | vector quantisation of pixel blocks |
| `wavelet-blur` | Indeo 5, Dirac, VC-2, Snow | block or full-frame wavelet transform |
| `lossless-reference` | HuffYUV, FFVHuff, Lagarith, Ut Video | lossless coding; they serve as the reference arm |
| `dct-blocking` | MPEG-1, MPEG-4 Part 2, H.263, MJPEG, DV, Theora, RealVideo, WMV, VC-1, VP6 (21 rows) | block DCT or integer block transform |

Two codecs are left out because their domain could not be verified: Indeo 2 (Huffman-coded deltas, no transform artifact class fits) and Smacker (the coding technique is not documented on the page checked).

## 7. The live upstream check

`scripts/codegen/check-encoder-upstreams.py` runs `vmafx_api/upstreams.py` against the network:

- Every `ffmpeg_encoder` must appear in FFmpeg master `libavcodec/allcodecs.c`. That file lists C symbols, where an encoder name such as `libaom-av1` is written `libaom_av1`; `SYMBOLS` holds the one name that differs further (`libvpx` is `ff_libvpx_vp8_encoder`, see `libavcodec/libvpxenc.c`).
- A row whose `upstream` is on GitHub, GitLab (`gitlab.com`, `code.videolan.org`), Gitiles (`*.googlesource.com`) or Bitbucket must state the maintenance class its last commit gives: active below 183 days, slow below 730 days, dead after, archived when the forge flags it.
- Where the forge reports an SPDX licence the checker's allow-list classifies, it must match `licence_class`. Judged classes (`patent-licensed-binary`, `proprietary-sdk`, `research-noncommercial`) are not overruled.
- Every other host, and every row without an upstream, is reported `unchecked`, never `ok`.

Result on 2026-10-29 (FFmpeg master and forge state of that day): **26 ok, 0 mismatch, 80 unchecked**, exit 0. The first run found three real defects, all fixed in this slice: the hyphen/underscore symbol mismatch (`libaom-av1`, `libvpx-vp9` reported missing), the `libvpx` alias, and `svt-vp9` wrongly classed as archived (it is slow). 76 of the 80 unchecked rows have no upstream repository (legacy and FFmpeg-native encoders). The other four are `av1_videotoolbox` (placeholder), `vtm` and `jvet-nnvc` (`vcgit.hhi.fraunhofer.de`, no API) and `libtheora` (`gitlab.xiph.org`, not in the GitLab host list).

Licence classes are mostly not forge-verified: most forges report no SPDX id for these projects. The six implemented software rows (`libx264`, `libx265`, `libsvtav1`, `libvvenc`, `libaom-av1`, `libvpx-vp9`) were checked by hand against each project's `LICENSE` or `COPYING` file.

## 8. The checker was seen failing

`scripts/codegen/tests/test_vmafx_api_upstreams.py` runs the checker against stub forges (positive, negative and boundary cases). Each of these mutations of `upstreams.py` made the suite fail:

| Mutation | Caught by |
|---|---|
| `ACTIVE_DAYS = 184` | maintenance edges |
| `<=` in the slow comparison | maintenance edges |
| licence check removed | licence contradicted |
| FFmpeg name check removed | encoder missing from FFmpeg |
| empty `allcodecs.c` accepted | allcodecs without encoders refused |
| `unchecked` reported as `ok` | host without forge API, row without upstream |
| `JUDGED` emptied | judged licence not overruled |
| archived flag ignored | archived wins over age |

The first mutation run proved nothing: the test module did not import the shared `support` helper, so it failed to import when run alone and every mutation showed one error. The rerun after the fix showed real assertion failures for all eight.

## 9. Not in this slice

Implementation of any gap row, the per-use-case ranking with its metric guard, and the epics and sub-issues per gap are tracked outside this digest.
