# Research digest 0054 — libFuzzer scaffold for the Y4M parser

- **Date**: 2026-05-03
- **Author**: Claude (Opus 4.7, 1M context) on behalf of lusoris
- **Companion ADR**: [ADR-0270](../adr/0270-fuzzing-scaffold.md)
- **Companion PR**: see CHANGELOG `### Added` row below

## Question

The OSSF Scorecard `Fuzzing` check is at 0/10 on this fork. What
is the smallest patch that takes it off zero, and which surface
should be the first harness?

## Method

1. Surveyed the libvmaf attack surface for parser-shaped inputs —
   anything that takes attacker-controllable bytes and runs
   `sscanf` / `memcpy` / size-derived `malloc` against them. Three
   candidates fell out:
    - `core/tools/y4m_input.c` — vendored Daala YUV4MPEG2 parser.
    - `core/src/read_json_model.c` — JSON model deserialiser.
    - `core/tools/yuv_input.c` — raw-YUV opener (driven by
      caller-supplied `width`, `height`, `pix_fmt` — fewer
      header-parsed code paths).
2. Read the OSSF Scorecard `Fuzzing` definition: any committed
   fuzz target counts (libFuzzer, AFL, OSS-Fuzz, native
   `go test -fuzz`, etc.). The grading is binary at the target-
   present threshold.
3. Compared the in-tree fuzz scaffolding patterns from peer C
   parsers — picked the libFuzzer pattern over AFL because clang
   ships libFuzzer by default and the build-host tooling burden
   is zero.
4. Sized the harness: `fmemopen` wraps the fuzzer's input bytes
   as a `FILE *`, the public `video_input_open` /
   `video_input_fetch_frame` / `video_input_close` triple drives
   the parser end-to-end without exposing the static-only
   `y4m_input_open_impl`.
5. Wrote a 60-second smoke run on the seed corpus to validate
   the build before opening the PR.

## Findings

- Y4M is the right first target. The JSON model loader is also
  parser-shaped, but the model files it reads are normally
  shipped alongside the binary, not received from the network or
  user input. The Y4M parser is invoked on `vmaf
  --reference path.y4m` — a path under attacker control if the
  attacker can write to a watched directory, drop a hostile file
  in a CI artefact share, or get a transcoding pipeline to feed
  the binary a crafted file.
- The Y4M parser is opted out of two SEI CERT rules at file scope
  (`bugprone-unchecked-string-to-number-conversion`,
  `cert-err34-c`) via a NOLINTBEGIN/NOLINTEND pair. The
  suppression preserves upstream verbatim text but it also
  papers over exactly the rule a fuzzer is best at exercising
  (sscanf returns < n on overflow, etc.). The harness gives us
  the coverage gate the lint rule was disabled to allow.
- libFuzzer's instrumentation flags (`-fsanitize=fuzzer`)
  conflict with `b_lundef=true` on clang (the warning is
  emitted at meson-setup time). The fuzz-only build sets
  `b_lundef=false` and that is documented in the fuzz README.
- The 60-second smoke run on the hand-crafted seed corpus
  (six seeds: 420, 422, 420p10, mono, 411, empty) hit a real
  heap-buffer-overflow within the first few seconds:
  `y4m_convert_411_422jpeg` writes `_dst[1]` unconditionally in
  its first sub-loop when `OC_MINI(c_w, 1) == 1` and the
  destination chroma width `dst_c_w == 1`. The third sub-loop
  guards the same write with `if ((x << 1 | 1) < dst_c_w) {…}`;
  the first does not. Reproducer:
  `YUV4MPEG2 W2 H4 F30:1 Ip C411\n` followed by a `FRAME\n`
  header and ~120 bytes of payload (full reproducer at
  `core/test/fuzz/y4m_input_known_crashes/y4m_411_w2_h4_oob_dst.y4m`).

## Conclusion

Land the libFuzzer scaffold + the Y4M harness now. Park the
411-chroma reproducer as a known-crash artefact (excluded from
nightly CI so the gate doesn't crash on PR-1) and open a
follow-up PR that ports the third-loop's `dst_c_w` guard to the
first and second sub-loops, then re-enables the reproducer as a
seed under `y4m_input_corpus/`.

## Smoke-test command

```bash
CC=clang CXX=clang++ \
  meson setup build-fuzz libvmaf \
    --buildtype=debug \
    -Db_sanitize=address \
    -Db_lundef=false \
    -Dfuzz=true \
    -Denable_cuda=false -Denable_sycl=false -Denable_vulkan=disabled
ninja -C build-fuzz test/fuzz/fuzz_y4m_input

mkdir -p /tmp/fuzz-smoke-y4m
./build-fuzz/test/fuzz/fuzz_y4m_input \
    -max_total_time=60 \
    -rss_limit_mb=2048 \
    -malloc_limit_mb=1024 \
    -timeout=10 \
    /tmp/fuzz-smoke-y4m \
    core/test/fuzz/y4m_input_corpus/

# Verify the known-crash reproducer triggers the same bug:
./build-fuzz/test/fuzz/fuzz_y4m_input \
    core/test/fuzz/y4m_input_known_crashes/y4m_411_w2_h4_oob_dst.y4m
```

The first run reaches `Done <N> runs in 60 second(s)` with no
crashes once the 411-OOB fix lands; the second always reproduces
the OOB until then.

## References

- ADR-0270 (decision matrix + alternatives).
- [OSSF Scorecard `Fuzzing` check](https://github.com/ossf/scorecard/blob/main/docs/checks.md#fuzzing).
- [libFuzzer guide (LLVM)](https://llvm.org/docs/LibFuzzer.html).
