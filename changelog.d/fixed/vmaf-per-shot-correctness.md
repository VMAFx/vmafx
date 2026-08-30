- `vmaf-perShot`: unknown CLI flags (typos, future options) now exit with a
  non-zero status and an "unrecognised option" error message instead of
  silently printing the help text and returning 0. Root cause: `--help` was
  mapped to the `'?'` short-option character, which getopt also uses as the
  "unknown option" sentinel, so any mistyped flag appeared to succeed.
  Remapped help to `-H` / `--help`.
- `vmaf-perShot`: replaced `fseek(fin, (long)chroma_bytes, SEEK_CUR)` with
  `fseeko` (POSIX) / `_fseeki64` (Win32) so the seek offset is 64-bit on all
  platforms. The previous `(long)` cast silently truncated for frames larger
  than 2 GiB on 32-bit targets, causing the chroma-skip to land at the wrong
  file position without returning an error.
