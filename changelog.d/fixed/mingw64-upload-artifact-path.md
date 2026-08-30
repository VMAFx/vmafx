- **`Build — Windows MinGW64 (CPU)` upload-artifact path fix.** The
  `stage` step computed `$(cygpath -m "$PWD/artifact/vmaf.exe")` and
  passed the result as the upload-artifact `path`. `upload-artifact@v7`
  resolves `path` relative to `GITHUB_WORKSPACE` and the absolute
  cygpath form was lost under MSYS path translation, surfacing as
  `if-no-files-found: error` while the stage step itself succeeded
  (master CI run `26683038580`). Pass the repo-relative path
  (`artifact/vmaf.exe`) directly instead; the action handles platform
  translation internally.
