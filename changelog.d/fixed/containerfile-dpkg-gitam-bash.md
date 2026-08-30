Fix two `dev/Containerfile` build-correctness bugs:

- **Package-verify false negatives**: the three GPU/VA-API install-verify loops
  used `apt-mark showmanual`, which (with the shared BuildKit `/var/lib/apt`
  cache mount) still flags a package as Auto-Installed even after an explicit
  `apt-get install`, spuriously failing the verify. Switched all three to
  `dpkg -l "${pkg}" | grep -q "^ii"` (installed-package DB), unaffected by the
  intent cache.
- **ffmpeg-patch loop masking failures**: a failed `git am --3way` ran
  `|| (… && exit 1)` in a subshell, so `exit 1` left the loop running and left
  `.git/rebase-apply` on disk, cascading "previous rebase directory still
  exists" into every later patch. Replaced with a brace group that runs
  `git am --abort` and propagates the failure.
