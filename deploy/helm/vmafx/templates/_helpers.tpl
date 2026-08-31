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
Resolve the Kubernetes device-plugin resource key for the requested GPU vendor.

Usage:
  resources:
    limits:
      {{ include "vmafx.gpuResource" . }}: {{ .Values.gpu.count | quote }}

Vendor → resource mapping:
  nvidia  →  nvidia.com/gpu       (CUDA + Vulkan-on-NVIDIA)
  amd     →  amd.com/gpu          (HIP  + Vulkan-on-AMD via ROCm)
  intel   →  gpu.intel.com/i915   (SYCL + Vulkan-on-Intel)
  cpu     →  (empty string — no device-plugin resource inserted)

Note: Vulkan is NOT a separate Kubernetes resource.  It runs through whichever
GPU device-plugin is allocated.  See docs/development/gpu-scheduling.md.
*/}}
{{- define "vmafx.gpuResource" -}}
{{- if .Values.gpu.enabled }}
{{- if eq .Values.gpu.vendor "nvidia" -}}
nvidia.com/gpu
{{- else if eq .Values.gpu.vendor "amd" -}}
amd.com/gpu
{{- else if eq .Values.gpu.vendor "intel" -}}
gpu.intel.com/i915
{{- else if eq .Values.gpu.vendor "cpu" -}}
{{- /* CPU workload: no device-plugin resource needed */ -}}
{{- else -}}
{{ fail (printf "gpu.vendor must be one of: nvidia, amd, intel, cpu — got %q" .Values.gpu.vendor) }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Resolve the VMAFX_BACKEND env-var value for the requested GPU vendor.
This tells the vmafx-server which backend to activate on startup.
*/}}
{{- define "vmafx.backendEnvValue" -}}
{{- if eq .Values.gpu.vendor "nvidia" -}}
cuda
{{- else if eq .Values.gpu.vendor "amd" -}}
hip
{{- else if eq .Values.gpu.vendor "intel" -}}
sycl
{{- else -}}
cpu
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
Resolve the Kubernetes device-plugin resource key (name only, no value).
Used in node.yaml where we need the key inline.
*/}}
{{- define "vmafx.gpuResourceKey" -}}
{{- if eq .Values.gpu.vendor "nvidia" -}}
nvidia.com/gpu
{{- else if eq .Values.gpu.vendor "amd" -}}
amd.com/gpu
{{- else if eq .Values.gpu.vendor "intel" -}}
gpu.intel.com/i915
{{- end }}
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
Resolve the vmafx-controller address for the node's VMAFX_CONTROLLER_ADDR env var.
Defaults to the in-cluster controller Service DNS name on port 8080.
Override via .Values.node.controllerAddr.
*/}}
{{- define "vmafx.controllerAddr" -}}
{{- if .Values.node.controllerAddr -}}
{{ .Values.node.controllerAddr }}
{{- else -}}
{{ include "vmafx.fullname" . }}-controller:8080
{{- end }}
{{- end }}

{{/*
Shared pod-spec fragments used by Deployment, Job, and StatefulSet.
Extracted here to avoid triplicating the container spec.
*/}}
{{- define "vmafx.containerSpec" -}}
- name: {{ include "vmafx.name" . }}
  image: {{ include "vmafx.image" . }}
  imagePullPolicy: {{ .Values.image.pullPolicy }}
  ports:
    - name: http
      containerPort: {{ .Values.service.targetPort }}
      protocol: TCP
  env:
    - name: VMAFX_BACKEND
      value: {{ include "vmafx.backendEnvValue" . | quote }}
  {{- range $k, $v := .Values.env }}
    - name: {{ $k | quote }}
      value: {{ $v | quote }}
  {{- end }}
  envFrom:
    - configMapRef:
        name: {{ include "vmafx.fullname" . }}-config
  {{- with .Values.envFrom }}
  {{- toYaml . | nindent 4 }}
  {{- end }}
  resources:
    limits:
      cpu: {{ .Values.resources.limits.cpu | quote }}
      memory: {{ .Values.resources.limits.memory | quote }}
    {{- if and .Values.gpu.enabled (include "vmafx.gpuResource" .) }}
      {{ include "vmafx.gpuResource" . }}: {{ .Values.gpu.count | quote }}
    {{- end }}
    requests:
      cpu: {{ .Values.resources.requests.cpu | quote }}
      memory: {{ .Values.resources.requests.memory | quote }}
  securityContext:
    {{- toYaml .Values.securityContext | nindent 4 }}
  volumeMounts:
    - name: tmp
      mountPath: /tmp
  {{- if .Values.persistence.corpus.enabled }}
    - name: corpus
      mountPath: {{ .Values.persistence.corpus.mountPath }}
      readOnly: true
  {{- end }}
  {{- if .Values.persistence.output.enabled }}
    - name: output
      mountPath: {{ .Values.persistence.output.mountPath }}
  {{- end }}
  {{- if .Values.persistence.models.enabled }}
    - name: models
      mountPath: {{ .Values.persistence.models.mountPath }}
      readOnly: true
  {{- end }}
{{- end }}

{{/*
Shared volume list used by all workload types.
*/}}
{{- define "vmafx.volumes" -}}
- name: tmp
  emptyDir: {}
{{- if .Values.persistence.corpus.enabled }}
- name: corpus
  persistentVolumeClaim:
    claimName: {{ include "vmafx.fullname" . }}-corpus
    readOnly: true
{{- end }}
{{- if .Values.persistence.output.enabled }}
- name: output
  persistentVolumeClaim:
    claimName: {{ include "vmafx.fullname" . }}-output
{{- end }}
{{- if .Values.persistence.models.enabled }}
- name: models
  persistentVolumeClaim:
    claimName: {{ include "vmafx.fullname" . }}-models
    readOnly: true
{{- end }}
{{- end }}

{{/*
Shared pod-level spec (security context, affinity, tolerations, etc.).
Rendered inside spec.template.spec for all workload types.
*/}}
{{- define "vmafx.podSpec" -}}
serviceAccountName: {{ include "vmafx.serviceAccountName" . }}
securityContext:
  {{- toYaml .Values.podSecurityContext | nindent 2 }}
{{- with .Values.imagePullSecrets }}
imagePullSecrets:
  {{- toYaml . | nindent 2 }}
{{- end }}
{{- with .Values.nodeSelector }}
nodeSelector:
  {{- toYaml . | nindent 2 }}
{{- end }}
{{- with .Values.affinity }}
affinity:
  {{- toYaml . | nindent 2 }}
{{- end }}
{{- with .Values.tolerations }}
tolerations:
  {{- toYaml . | nindent 2 }}
{{- end }}
{{- with .Values.topologySpreadConstraints }}
topologySpreadConstraints:
  {{- toYaml . | nindent 2 }}
{{- end }}
containers:
  {{- include "vmafx.containerSpec" . | nindent 2 }}
volumes:
  {{- include "vmafx.volumes" . | nindent 2 }}
{{- end }}
