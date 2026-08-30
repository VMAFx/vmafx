- Drop the broken `filter=lfs diff=lfs merge=lfs -text` declaration on
  `model/tiny/*.onnx` from `.gitattributes`. The blobs were never pushed
  to LFS storage, so smudge in fresh worktrees produced 130-byte pointer
  text instead of the real binaries, derailing every agent worktree
  created during the 2026-05-15 audit / gap-fill session. Files now
  declared `binary -filter -diff -merge` (matching the existing
  `dists_sq.onnx` carve-out). Real LFS migration requires
  `git lfs migrate import` + a master force-push (blocked by
  ADR-0037 branch protection); deferred to a separate planned operation
  with its own ADR.
