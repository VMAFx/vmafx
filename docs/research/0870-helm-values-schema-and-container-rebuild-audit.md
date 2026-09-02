<!-- markdownlint-disable MD060 -->
# Research-0870: Helm values.schema.json + dev-MCP Containerfile rebuild audit

- **Date**: 2026-05-30
- **Author**: Lusoris (audit-dispatched agent)
- **Cites**: [ADR-0870](../adr/0870-helm-values-schema-and-container-rebuild-audit.md),
  [ADR-0700](../adr/0700-vmafx-repo-layout.md),
  [ADR-0699](../adr/0699-vmafx-helm-chart-k8s.md),
  [ADR-0703](../adr/0703-vmafx-server-go-grpc.md),
  [ADR-0714](../adr/0714-vmafx-operator-skeleton.md),
  [ADR-0719](../adr/0719-vmafx-node-rclone-integration.md),
  [ADR-0726](../adr/0726-drop-vulkan-backend.md)

## 1. Scope

Audit dispatched 2026-05-30 against `master` tip `bbcaa8d127`. Two
adjacent operational-hygiene questions:

1. Does `deploy/helm/vmafx/values.yaml` carry a JSON Schema to catch
   typos at install time?
2. Per CLAUDE.md §15, the `vmaf-dev-mcp` container at `dev/Containerfile`
   should rebuild clean when master changes under `core/`, `mcp-server/`,
   `ai/`, `tools/vmaf-tune/`, or `dev/`. Has it drifted?

## 2. Method

- **Helm**: read `values.yaml`, compute its top-level surface, check for
  the conventional sidecar `values.schema.json`, run `helm lint
  --strict` and `helm template`. Test schema enforcement by attempting
  rendering with an invalid `gpu.vendor`.
- **Container**: read the Containerfile end-to-end, run `hadolint
  dev/Containerfile`, grep `dev/` and `scripts/` for path references
  that the post-ADR-0700 rename would have broken, cross-check the
  source-tree state (does `libvmaf/` still exist? where is `core/`?).
- Read ADR-0700 to confirm the rename's authoritative scope.

## 3. Findings

### 3.1 Helm chart: schema absent

`deploy/helm/vmafx/values.schema.json` does not exist. The chart's
`values.yaml` is 362 lines, exposes 25 top-level keys, and includes
three load-bearing enum fields:

| Field | Enum | Where consumed |
| --- | --- | --- |
| `workload` | `Deployment` \| `Job` \| `StatefulSet` | `templates/deployment.yaml`, `templates/job.yaml`, `templates/statefulset.yaml` (selected via Helm template branch) |
| `gpu.vendor` | `nvidia` \| `amd` \| `intel` \| `cpu` | `templates/_helpers.tpl` maps to `nvidia.com/gpu` / `amd.com/gpu` / `gpu.intel.com/i915` / `<none>` |
| `storage.mode` | `http-serve` \| `rclone` | `templates/node-deployment.yaml`, `templates/secret-rclone-config.yaml` (rclone Secret only created when `mode == "rclone"`) |

Without a schema, `helm install vmafx --set workload=Daemonset` is
silently accepted; the chart's `{{ if eq .Values.workload "Deployment"
}}` / Job / StatefulSet branches all evaluate false and zero workloads
are created. The same happens for `gpu.vendor=qualcomm`: the
`_helpers.tpl` GPU vendor `default` branch fires (no resource key) and
the operator only notices when pods land on CPU nodes despite asking
for an NVIDIA backend.

`helm lint --strict` passes (no schema, no schema violations). Server
validation via `helm template --validate` cannot run without cluster
reachability (audit machine has no kube context).

### 3.2 Containerfile: ADR-0700 path drift

ADR-0700 renamed `libvmaf/` → `core/` (and `python/vmaf/` →
`compat/python-vmaf/` with a shim). Verified by `ls`:

```text
$ ls libvmaf/
ls: cannot access 'libvmaf/': No such file or directory

$ ls core/meson.build core/meson_options.txt
core/meson.build  core/meson_options.txt
```

The Containerfile (last touched 2026-04-26 via PR #239 for the Python
side of the same rename) still has these stale references:

| File | Line | Bad reference |
| --- | --- | --- |
| `dev/Containerfile` | 151 | comment: `libvmaf/src/meson.build` |
| `dev/Containerfile` | 434 | comment: `libvmaf/        — meson source` |
| `dev/Containerfile` | 452 | `COPY --chown=vmaf:vmaf libvmaf/ /build/vmaf/libvmaf/` |
| `dev/Containerfile` | 485 | comment: `meson.build lives in libvmaf/` |
| `dev/Containerfile` | 515 | `RUN cd libvmaf && CC=icx CXX=icpx meson setup build …` |
| `dev/Containerfile` | 533 | `RUN cd libvmaf && ninja -C build install` |
| `.dockerignore`     | 32-34 | `libvmaf/build*/` siblings only; no `core/build*/` |

Against current master, the COPY on line 452 fails with `"libvmaf":
not found` and the build aborts before reaching CUDA / SYCL / HIP
stages. This breaks every fresh `docker compose build dev-mcp`
invocation — the standard CLAUDE.md §15 rebuild path.

### 3.3 hadolint findings (informational)

Pre-existing, non-HIGH-severity:

- DL3003 (`cd` in RUN, 5 occurrences) — accepted multi-stage idiom
  for the ROCm/NEO/SVT-AV1/VVenC source-build layers where `WORKDIR`
  would break the `&&`-chained `rm -rf` cleanup. Suppressing each
  in-line is a separate cleanup pass.
- DL4006 (SHELL pipefail not set, 2 occurrences) — both occur on
  `apt-mark` verification RUNs that don't actually pipe. The stage's
  SHELL directive sets pipefail; hadolint can't follow cross-stage
  inheritance per the existing `dev/AGENTS.md §SHELL / hadolint
  DL4006` entry.
- DL3002 (last USER root, 1 occurrence) — forced by the FFmpeg `make
  install` step that writes to `/usr/local/`.
- DL3009 (apt lists not deleted, 1 occurrence) — false positive on
  the NEO compute-runtime layer that explicitly `find /var/lib/apt/
  lists -mindepth 1 -delete` at the end.

None are HIGH severity; none block the build; out of audit scope per
the task brief.

### 3.4 Base images: not EOL

- `ubuntu:26.04@sha256:f3d28607ddd78...` — Resolute Raccoon, current
  LTS-track release, supported by Canonical.
- NVIDIA CUDA repo: `ubuntu2404` channel (intentional cross-distro pin
  per the comment block on line 130-156). Not EOL.
- Intel oneAPI apt repo: rolling.
- AMD ROCm: `noble` channel pinned at 7.2.3. Active.
- ONNX Runtime: 1.26.0 (current).
- SVT-AV1: 4.1.0. VVenC: 1.14.0. AMF: 1.5.2. All current as of
  ADR-0568 (SDK audit 2026-05-18).

No EOL base images.

## 4. Decision

Land both fixes in a single PR. The Containerfile drift is a hard
build break; the schema gap is the highest-leverage typo-catching
addition for an operator-facing surface. They share the audit cycle
and the ADR-0108 deliverables.

See ADR-0870 for the decision text.

## 5. Reproducer

```bash
# Schema enforcement (post-fix)
helm template deploy/helm/vmafx --set gpu.vendor=qualcomm
# → Error: values don't meet the specifications of the schema(s) …
# → at '/gpu/vendor': value must be one of 'nvidia', 'amd', 'intel', 'cpu'

# Container build (post-fix)
docker compose --project-directory $(git rev-parse --show-toplevel) \
    -f dev/docker-compose.yml build dev-mcp
# → Successful COPY core/ and COPY compat/, meson configure in core/
#   proceeds to CUDA / SYCL / HIP / Metal stages without "libvmaf: not
#   found"

# Hadolint (no new findings beyond the pre-existing advisories)
hadolint dev/Containerfile
# → 8 pre-existing DL3003/DL4006/DL3002/DL3009 advisories; zero HIGH.
```

## 6. References

- req: audit dispatch 2026-05-30 ("Audit Helm chart values schema
  enforcement + verify the vmaf-dev-mcp container image still builds
  clean against current master").
- ADR-0700 (`docs/adr/0700-vmafx-repo-layout.md`) — the `libvmaf/`
  → `core/` rename that the Containerfile drift came from.
- ADR-0870 (`docs/adr/0870-helm-values-schema-and-container-rebuild-audit.md`) —
  the decision this digest informs.
- CLAUDE.md §15 — `vmaf-dev-mcp` rebuild policy enforced by this
  audit.
