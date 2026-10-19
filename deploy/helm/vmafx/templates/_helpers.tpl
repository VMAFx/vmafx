{{/*
VMAFX Helm chart helpers.
Ref: https://helm.sh/docs/chart_template_guide/named_templates/
*/}}

{{/*
Expand the name of the chart.
*/}}
{{- define "vmafx.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
Truncated at 63 characters because some Kubernetes name fields are limited.
*/}}
{{- define "vmafx.fullname" -}}
{{- if .Values.fullnameOverride }}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- $name := default .Chart.Name .Values.nameOverride }}
{{- if contains $name .Release.Name }}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Create chart label value.
*/}}
{{- define "vmafx.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels applied to every resource.
*/}}
{{- define "vmafx.labels" -}}
helm.sh/chart: {{ include "vmafx.chart" . }}
{{ include "vmafx.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels used by Service and workload selectors.
*/}}
{{- define "vmafx.selectorLabels" -}}
app.kubernetes.io/name: {{ include "vmafx.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Service account name.
*/}}
{{- define "vmafx.serviceAccountName" -}}
{{- if .Values.serviceAccount.create }}
{{- default (include "vmafx.fullname" .) .Values.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.serviceAccount.name }}
{{- end }}
{{- end }}

{{/*
Resolve the Kubernetes device-plugin resource name for the requested GPU.

Usage:
  resources:
    limits:
      {{ include "vmafx.gpuResource" . }}: {{ .Values.gpu.count | quote }}

Resolution (vmafx.gpuResourceName, one definition for every workload):
  gpu.resourceName set  →  that name, verbatim (any device plugin or sharing
                           mode, e.g. gpu.intel.com/xe, nvidia.com/mig-1g.10gb)
  nvidia                →  nvidia.com/gpu                    (CUDA backend)
  amd                   →  amd.com/gpu                       (HIP backend via ROCm)
  intel                 →  gpu.intel.com/<gpu.intelDriver>   (SYCL backend;
                           i915 by default, xe for GPUs on the xe kernel driver)
  cpu                   →  (empty — no device-plugin resource inserted)

vmafx.gpuResource is the same name, empty when gpu.enabled is false.
Note: Vulkan was removed as a VMAFX backend in ADR-0726.
See docs/development/gpu-scheduling.md.
*/}}
{{- define "vmafx.gpuResourceName" -}}
{{- $vendor := .Values.gpu.vendor -}}
{{- if and .Values.gpu.resourceName (eq $vendor "cpu") -}}
{{ fail "gpu.resourceName needs a GPU vendor; gpu.vendor is cpu" }}
{{- else if .Values.gpu.resourceName -}}
{{ .Values.gpu.resourceName }}
{{- else if eq $vendor "nvidia" -}}
nvidia.com/gpu
{{- else if eq $vendor "amd" -}}
amd.com/gpu
{{- else if eq $vendor "intel" -}}
gpu.intel.com/{{ .Values.gpu.intelDriver | default "i915" }}
{{- else if eq $vendor "cpu" -}}
{{- /* CPU workload: no device-plugin resource needed */ -}}
{{- else -}}
{{ fail (printf "gpu.vendor must be one of: nvidia, amd, intel, cpu — got %q" $vendor) }}
{{- end }}
{{- end }}

{{- define "vmafx.gpuResource" -}}
{{- if .Values.gpu.enabled }}
{{- include "vmafx.gpuResourceName" . }}
{{- end }}
{{- end }}

{{/*
Render the Go server container image reference (repository:tag).
Falls back to the canonical published release tag when .Values.image.tag is empty.
*/}}
{{- define "vmafx.image" -}}
{{- $tag := .Values.image.tag | default (include "vmafx.releaseImageTag" .) -}}
{{- printf "%s:%s" .Values.image.repository $tag -}}
{{- end }}

{{/*
Render the canonical release tag used by the operator and node publishers.
GitHub releases are tagged vX.Y.Z while Chart.AppVersion intentionally stores
the bare SemVer value for Kubernetes labels. Strip a pre-existing prefix so a
future v-prefixed AppVersion cannot render vvX.Y.Z.
*/}}
{{- define "vmafx.releaseImageTag" -}}
{{- printf "v%s" (.Chart.AppVersion | toString | trimPrefix "v") -}}
{{- end }}

{{/*
Render the vmafx-operator image reference. Explicit user tags remain verbatim;
the chart default follows the canonical tag published by the release workflow.
*/}}
{{- define "vmafx.operatorImage" -}}
{{- $tag := .Values.operator.image.tag | default (include "vmafx.releaseImageTag" .) -}}
{{- printf "%s:%s" .Values.operator.image.repository $tag -}}
{{- end }}

{{/*
Render the vmafx-node container image reference.
The shipped values name the release repository explicitly. The repository
fallback remains for custom chart values that intentionally derive a sibling
node package. Override via .Values.node.image.repository / .Values.node.image.tag.
*/}}
{{- define "vmafx.nodeImage" -}}
{{- $repo := .Values.node.image.repository | default (printf "%s-node" .Values.image.repository) -}}
{{- $tag  := .Values.node.image.tag        | default (include "vmafx.releaseImageTag" .) -}}
{{- printf "%s:%s" $repo $tag -}}
{{- end }}

{{/*
vmafx.tenantSource — "kubernetes" when the controller reads its tenants from
VmafxTenant resources (auth.tenantSource, implied by a non-empty auth.tenants),
else "". ADR-1519.
*/}}
{{- define "vmafx.tenantSource" -}}
{{- if .Values.auth.tenantSource -}}
{{ .Values.auth.tenantSource }}
{{- else if .Values.auth.tenants -}}
kubernetes
{{- end -}}
{{- end }}

{{/*
The controller's own service account (ADR-1592): the only account bound to
the VmafxTenant reader Role. The server, job and node pods keep
vmafx.serviceAccountName, which holds no RBAC.
*/}}
{{- define "vmafx.controllerServiceAccountName" -}}
{{- printf "%s-controller" (include "vmafx.serviceAccountName" .) -}}
{{- end }}

{{/*
vmafx-controller image (ADR-1589); the tag defaults to the release tag.
*/}}
{{- define "vmafx.controllerImage" -}}
{{- $tag := .Values.controller.image.tag | default (include "vmafx.releaseImageTag" .) -}}
{{- printf "%s:%s" .Values.controller.image.repository $tag -}}
{{- end }}

{{/*
In-cluster host of the chart's controller Service.
*/}}
{{- define "vmafx.controllerHost" -}}
{{- printf "%s-controller.%s.svc" (include "vmafx.fullname" .) .Release.Namespace -}}
{{- end }}

{{/*
gRPC address the nodes use: node.controllerAddr, else the chart's controller
when controller.enabled, else empty (standalone nodes).
*/}}
{{- define "vmafx.nodeControllerAddr" -}}
{{- if .Values.node.controllerAddr -}}
{{ .Values.node.controllerAddr }}
{{- else if .Values.controller.enabled -}}
{{ printf "%s:%v" (include "vmafx.controllerHost" .) .Values.controller.grpcPort }}
{{- end -}}
{{- end }}

{{/*
Controller token volume and mount (vmafx.controllerTokenVolume /
vmafx.controllerTokenMount) for a workload whose <component>.controllerToken
names a Secret; call with (dict "token" .Values.node.controllerToken).
*/}}
{{- define "vmafx.controllerTokenPath" -}}
/var/run/secrets/vmafx/controller-token/token
{{- end }}
{{- define "vmafx.controllerTokenVolume" -}}
{{- with .token.secretName }}
- name: controller-token
  secret:
    secretName: {{ . }}
    defaultMode: 0400
    items:
      - key: {{ $.token.key | default "token" }}
        path: token
{{- end }}
{{- end }}
{{- define "vmafx.controllerTokenMount" -}}
{{- if .token.secretName }}
- name: controller-token
  mountPath: /var/run/secrets/vmafx/controller-token
  readOnly: true
{{- end }}
{{- end }}

{{/*
vmafx.nodeMountRoot — VMAFX_STORAGE_MOUNT_ROOT of the node pods: storage.mountRoot,
or with the eBPF tracker on its mount prefix without the trailing slash
(ADR-1593). Empty: the node's temp dir.
*/}}
{{- define "vmafx.nodeMountRoot" -}}
{{- if .Values.storage.mountRoot -}}
{{- .Values.storage.mountRoot -}}
{{- else if .Values.node.ebpf.enabled -}}
{{- trimSuffix "/" .Values.node.ebpf.mountPrefix -}}
{{- end -}}
{{- end }}

{{/*
vmafx.nodeMountRootVolume — "true" when the node pods need an emptyDir at the
mount root: FUSE on and a mount root outside /tmp (the /tmp emptyDir holds
the temp-dir default).
*/}}
{{- define "vmafx.nodeMountRootVolume" -}}
{{- $root := include "vmafx.nodeMountRoot" . -}}
{{- if and .Values.node.fuse.enabled $root (ne $root "/tmp") (not (hasPrefix "/tmp/" $root)) -}}
true
{{- end -}}
{{- end }}

{{/*
vmafx.nodeSecurityContext — the node container's securityContext (ADR-1593):
.Values.securityContext, and with node.fuse the capability bounding set the
setuid fusermount3 mounts with (SYS_ADMIN, DAC_READ_SEARCH; the node process
itself keeps no effective capability) plus allowPrivilegeEscalation, which a
setuid helper needs. With node.ebpf the container runs as UID 0 (a non-root
process has no effective capabilities) with BPF, PERFMON and SYS_ADMIN only.
*/}}
{{- define "vmafx.nodeSecurityContext" -}}
{{- $sc := deepCopy .Values.securityContext -}}
{{- if .Values.node.fuse.enabled -}}
{{- $add := list "SYS_ADMIN" "DAC_READ_SEARCH" -}}
{{- if .Values.node.ebpf.enabled -}}
{{- $add = list "BPF" "PERFMON" "SYS_ADMIN" -}}
{{- $_ := set $sc "runAsUser" 0 -}}
{{- $_ := set $sc "runAsNonRoot" false -}}
{{- end -}}
{{- $_ := set $sc "allowPrivilegeEscalation" true -}}
{{- $_ := set $sc "capabilities" (dict "drop" (list "ALL") "add" $add) -}}
{{- with .Values.node.fuse.appArmorProfile -}}
{{- $_ := set $sc "appArmorProfile" . -}}
{{- end -}}
{{- end -}}
{{- toYaml $sc -}}
{{- end }}

{{/*
A ServiceMonitor or PodMonitor of one component, with the scrape settings of
monitoring.serviceMonitor (templates/servicemonitor.yaml). Arguments: root,
kind (ServiceMonitor or PodMonitor), name, component, port. A monitor in
another namespace than the release selects the release namespace; a
ServiceMonitor skips the StatefulSet's headless Service.
*/}}
{{- define "vmafx.monitor" -}}
{{- $root := .root -}}
{{- $sm := $root.Values.monitoring.serviceMonitor -}}
apiVersion: monitoring.coreos.com/v1
kind: {{ .kind }}
metadata:
  name: {{ .name }}
  namespace: {{ $sm.namespace | default $root.Release.Namespace }}
  labels:
    {{- include "vmafx.labels" $root | nindent 4 }}
    app.kubernetes.io/component: {{ .component }}
    {{- with $sm.labels }}
    {{- toYaml . | nindent 4 }}
    {{- end }}
spec:
  {{- if and $sm.namespace (ne $sm.namespace $root.Release.Namespace) }}
  namespaceSelector:
    matchNames:
      - {{ $root.Release.Namespace }}
  {{- end }}
  selector:
    matchLabels:
      {{- include "vmafx.selectorLabels" $root | nindent 6 }}
      app.kubernetes.io/component: {{ .component }}
    {{- if eq .kind "ServiceMonitor" }}
    # A StatefulSet's headless Service selects the same pods.
    matchExpressions:
      - key: vmafx.dev/headless
        operator: DoesNotExist
    {{- end }}
  {{ if eq .kind "PodMonitor" }}podMetricsEndpoints{{ else }}endpoints{{ end }}:
    - port: {{ .port }}
      path: {{ $sm.path }}
      scheme: {{ $sm.scheme }}
      interval: {{ $sm.interval }}
      scrapeTimeout: {{ $sm.scrapeTimeout }}
      honorLabels: {{ $sm.honorLabels }}
{{- end }}

{{/*
vmafx.controllerStoreBackend — controller.store.backend after its checks
(ADR-2350): sqlite or postgres; more than one replica needs postgres, and
postgres in external mode needs the Secret that holds the connection string.
*/}}
{{- define "vmafx.controllerStoreBackend" -}}
{{- $store := .Values.controller.store -}}
{{- if not (has $store.backend (list "sqlite" "postgres")) -}}
{{- fail (printf "controller.store.backend: %q is neither sqlite nor postgres" $store.backend) -}}
{{- end -}}
{{- if lt (int .Values.controller.replicas) 1 -}}
{{- fail "controller.replicas: at least 1" -}}
{{- end -}}
{{- if and (gt (int .Values.controller.replicas) 1) (ne $store.backend "postgres") -}}
{{- fail "controller.replicas above 1 needs controller.store.backend postgres: two controllers on the SQLite backend would each own a different queue" -}}
{{- end -}}
{{- if eq $store.backend "postgres" -}}
{{- if not (has $store.postgresql.mode (list "cnpg" "external")) -}}
{{- fail (printf "controller.store.postgresql.mode: %q is neither cnpg nor external" $store.postgresql.mode) -}}
{{- end -}}
{{- if and (eq $store.postgresql.mode "external") (not $store.postgresql.external.secretName) -}}
{{- fail "controller.store.postgresql.external.secretName: the Secret holding the database connection string is required in external mode" -}}
{{- end -}}
{{- end -}}
{{- $store.backend -}}
{{- end }}

{{/*
vmafx.controllerDatabaseCluster — name of the CloudNativePG Cluster the chart
renders in cnpg mode.
*/}}
{{- define "vmafx.controllerDatabaseCluster" -}}
{{- printf "%s-db" (include "vmafx.fullname" .) -}}
{{- end }}

{{/*
vmafx.controllerTopologySpread — controller.topologySpreadConstraints, or with
more than one replica and none set a soft spread over Kubernetes nodes.
*/}}
{{- define "vmafx.controllerTopologySpread" -}}
{{- if .Values.controller.topologySpreadConstraints -}}
topologySpreadConstraints:
  {{- toYaml .Values.controller.topologySpreadConstraints | nindent 2 }}
{{- else if gt (int .Values.controller.replicas) 1 -}}
topologySpreadConstraints:
  - maxSkew: 1
    topologyKey: kubernetes.io/hostname
    whenUnsatisfiable: ScheduleAnyway
    labelSelector:
      matchLabels:
        {{- include "vmafx.selectorLabels" . | nindent 8 }}
        app.kubernetes.io/component: controller
{{- end -}}
{{- end }}

{{/*
vmafx.controllerMigrationPod — pod template of the migration Job
(controller-database.yaml), which also names the Job by its hash.
*/}}
{{- define "vmafx.controllerMigrationPod" -}}
metadata:
  labels:
    {{- include "vmafx.selectorLabels" . | nindent 4 }}
    app.kubernetes.io/component: migrate
spec:
  restartPolicy: OnFailure
  serviceAccountName: {{ include "vmafx.controllerServiceAccountName" . }}
  automountServiceAccountToken: false
  securityContext:
    {{- toYaml .Values.podSecurityContext | nindent 4 }}
  {{- with .Values.imagePullSecrets }}
  imagePullSecrets:
    {{- toYaml . | nindent 4 }}
  {{- end }}
  containers:
    - name: migrate
      image: {{ include "vmafx.controllerImage" . | quote }}
      imagePullPolicy: {{ .Values.controller.image.pullPolicy | default "IfNotPresent" }}
      args: ["migrate"]
      env:
        {{- include "vmafx.env.migrate" . }}
      resources:
        {{- toYaml .Values.controller.resources | nindent 8 }}
      securityContext:
        {{- toYaml .Values.securityContext | nindent 8 }}
{{- end }}
