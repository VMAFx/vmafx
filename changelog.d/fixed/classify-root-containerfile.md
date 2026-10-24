- The dependency pull request classifier treats `Containerfile` and
  `Containerfile.*` in any directory as container manifests, as it already
  treats `Dockerfile*`, so a Renovate digest bump of the root
  `Containerfile.vmafx` no longer fails the deliverables checklist.
