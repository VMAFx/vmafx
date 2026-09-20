- **Pelorus v0.2.2 interop blobs no longer trigger undefined behavior when the
  caller's byte buffer has a misaligned base.** The vendored parser now moves
  wire headers and directory entries through aligned locals with `memcpy` and
  rejects a `header_size` that would misalign the section directory. ABI 1.3 is
  unchanged. The shared fixture is again exact Pelorus source (16 vectors), and
  a required fail-closed drift check plus VMAFx-side lint exclusions prevent
  local fixture edits from diverging again.
