- **`cambi` no longer reads and writes outside its buffers on wide, short
  frames.** When the coarsest of CAMBI's five scales had fewer rows than half
  the window (e.g. up to 160 rows at 1920 wide, up to 224 rows at 2560 wide, and
  up to 336 rows at 3840 wide, such as 1920x64, 1920x128, 1920x160, 3840x128,
  and 3840x256), the c-values pass ran past both ends of the frame: AddressSanitizer
  reported heap-buffer-overflows and release builds could crash. The scalar,
  AVX2, AVX-512 and NEON paths now clip the window to the rows that exist, as the
  upstream fix does ([Netflix/vmaf#1628](https://github.com/Netflix/vmaf/issues/1628),
  [Netflix/vmaf#1629](https://github.com/Netflix/vmaf/pull/1629)). Scores change for
  frames with fewer than `pad_size + 1` rows at the coarsest scale that previously
  completed on the scalar path while reading out of bounds (measured on 3-frame
  8-bit 4:2:0 ramps: 3840x128 moves from 19.544347 to 19.512269, 1920x160 from
  22.270340 to 22.267602, and 3840x256 from 21.778393 to 21.769946), and are
  unchanged for all other frame sizes that have at least `pad_size + 1` rows at every
  scale ([CAMBI frame sizes](docs/metrics/cambi.md#frame-sizes)).
