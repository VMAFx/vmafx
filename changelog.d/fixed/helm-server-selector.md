- **Helm: the server Deployment no longer selects the operator and node Pods.**
  The chart's server Deployment and StatefulSet selected only the release
  labels, so they also matched the operator, node and `helm test` Pods, and
  `kubectl logs deployment/vmafx` could print the operator's log. Both now
  also select `app.kubernetes.io/component: server` (ADR-1353). Scoring
  traffic was not affected: the Services already selected the server Pods
  only. **Upgrade note for v1.0.0-rc.1 installs:** a workload's selector
  cannot be changed in place, so `helm upgrade` fails with
  `spec.selector: ... field is immutable`. Delete the server workload first;
  `--cascade=orphan` keeps its Pods serving until the upgrade replaces them:
  `kubectl delete deployment,statefulset -n <namespace> --cascade=orphan -l app.kubernetes.io/instance=<release>,app.kubernetes.io/component=server`,
  then run `helm upgrade` as usual. Uninstalling and installing again also
  works. See "Upgrading from 1.0.0-rc.1" in
  `docs/development/k8s-deployment.md`.
