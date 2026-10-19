- **The operator's events reach every namespace it reconciles
  ([ADR-2647](docs/adr/2647-operator-events-cluster-wide.md)).** The chart
  allowed the operator to create events only in the release namespace, but
  Kubernetes stores an event in the namespace of the resource it describes,
  so the `CheckpointWritten` event of a `VmafxModelTraining` in another
  namespace was refused and lost. A new `<release>-operator-events`
  `ClusterRole` grants `create` and `patch` on events in every namespace and
  nothing else; pods and the leader-election lease stay in the release
  namespace's `Role`.
