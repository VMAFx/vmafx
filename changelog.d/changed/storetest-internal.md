- **The controller store's test helper moved to
  `cmd/vmafx-controller/internal/storetest`.** Only tests ever imported the
  package. Digest updates of its PostgreSQL images no longer read as an
  incompatible API change.
