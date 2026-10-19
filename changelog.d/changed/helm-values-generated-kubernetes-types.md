- **The Helm chart's values schema checks Kubernetes fields with the
  Kubernetes 1.26 types, and the values file and schema are generated
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** `values.yaml` and
  `values.schema.json` are written from `api/vmafx-platform.toml`;
  `values.yaml` is unchanged and the schema keeps its rules in a uniform
  layout. The values the chart copies into pod specs now have the types the
  oldest supported Kubernetes release gives them: `tolerations`, `affinity`,
  `topologySpreadConstraints`, `podSecurityContext`, `securityContext`, the
  probes, `node.volumes` and `node.volumeMounts`, the update strategies,
  `envFrom`, `ingress.tls`, node selectors, labels and annotations. `helm
  install`, `helm upgrade` and `helm lint` now refuse a value Kubernetes would
  refuse, such as `tolerationSeconds: "60"`, a spread constraint without
  `topologyKey` or a node selector value `1`, and name the key; they check it
  even when the workload that uses it is disabled. The upgrade notes in
  `docs/development/k8s-deployment.md` list what each key refuses. Resource
  quantities may now be decimal numbers. The chart now declares
  `artifacthub.io/license: EUPL-1.2 AND Apache-2.0`, because the schema
  carries the Apache-2.0 Kubernetes type schemas, and ships
  `THIRD-PARTY-NOTICES.txt` with their attribution and the Apache-2.0 text
  ([ADR-2673](docs/adr/2673-chart-licence-kubernetes-schemas.md)).
