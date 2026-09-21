- Consolidated nine duplicated Go implementation families behind shared
  service, model, backend, registry, and Kubernetes deep-copy owners; HTTP
  response and scratch-cleanup failures are now logged instead of silently
  discarded, a missing score sidecar fails explicitly, and the dedicated clone
  scan now blocks local commits/pushes and required CI.
