**vmaf-tune ladder:** decode container/Y4M references to raw YUV before
scoring (ADR-0499). Previously `vmaf-tune ladder --src foo.mp4 …`
failed with `RuntimeError: default sampler produced no scorable
encodes` because only the *distorted* leg of the score call was
decoded — the reference (`.mp4`/`.y4m`) went straight to the libvmaf
CLI, which interpreted the container bytes as raw planar YUV and
aborted with "file size mismatch". `vmaftune.corpus.iter_rows` now
decodes the reference once per sweep into a `.ref.decoded.yuv`
sidecar under the encode dir and reuses it across every (preset,
crf) cell. Drop `.y4m` from `_VMAF_RAW_SUFFIXES` (corpus) and
`VMAF_RAW_SUFFIXES` (score): vmaf-tune always emits
`--width`/`--height`/`--pixel_format`/`--bitdepth` which flips the
CLI's `use_yuv` flag (core/tools/cli_parse.c), so `.y4m` never
reaches the Y4M parser. 9 new regression tests including a cross-check
against `cli_parse.c` so future CLI changes to the `use_yuv`
discipline are caught at lint-test time.
