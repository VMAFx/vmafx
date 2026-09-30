- **`cambi` no longer reads and writes outside its buffers on wide, short
  frames, and scores tall, narrow frames the same on every path.** When the
  coarsest of CAMBI's five scales had no more rows than half the window,
  rounded down (`pad_size`; with the default window every height up to 176 at
  1920 wide, 240 at 2560 wide and 352 at 3840 wide), the c-values pass ran past
  both ends of the frame: AddressSanitizer reported heap-buffer-overflows and
  release builds could crash. The scalar, AVX2, AVX-512 and NEON paths now
  clip the window to the rows that exist, as the upstream fix does
  ([Netflix/vmaf#1628](https://github.com/Netflix/vmaf/issues/1628),
  [Netflix/vmaf#1629](https://github.com/Netflix/vmaf/pull/1629)). Frames with
  fewer than `pad_size` rows at that scale (up to 160, 224 and 336 rows at
  those widths) score differently where they completed before; for example a
  3840x128 horizontal ramp moves from 19.544347 to 19.512269 on the C path.
  Going beyond upstream Netflix/vmaf#1629, which bounds only the rows, the
  columns are bounded too (`MIN(pad_size, width)` in the four left-edge loops):
  scores change for frames narrower than `pad_size` at some scale (measured:
  64x1920 vertical ramp master 14.975700714938673 vs branch 14.964394451743877
  on the C path; the SIMD paths already agreed). On frames with fewer than
  `pad_size` columns at that scale (up to 80 wide at 1080 high, 160 at 1920
  high) the scalar walk read columns past the frame. Those frames now score on
  the C path (`--cpumask 63`, builds without SIMD) and on the CUDA, HIP and
  Metal twins, which run that walk on the host, what the default dispatch
  already gave (64x1920 vertical ramp moves from 14.975700714938673 to
  14.964394451743877; another vertical ramp variant moves from 16.141046 to
  16.131541). All other frame sizes score as before
  ([CAMBI frame sizes](docs/metrics/cambi.md#frame-sizes)).
