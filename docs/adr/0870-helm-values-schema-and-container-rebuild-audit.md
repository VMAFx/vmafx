# ADR-0870: Helm chart values.schema.json + dev-MCP Containerfile rebuild audit

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: helm, k8s, deploy, devx, dev-mcp, container, rebase-hygiene

## Context

Two adjacent operational-hygiene gaps surfaced during the audit window
on 2026-05-30:

1. **Helm chart had no `values.schema.json`.**
   `deploy/helm/vmafx/values.yaml` carries ~25 top-level keys and three
   load-bearing enums (`workload` ∈ {Deployment, Job, StatefulSet},
   `gpu.vendor` ∈ {nvidia, amd, intel, cpu}, `storage.mode` ∈
   {http-serve, rclone}). Without a schema, a typo like
   `--set gpu.vendor=qualcomm` or `--set workload=Daemonset` is accepted
   silently at install time; the chart renders a manifest that either
   schedules onto a non-existent device-plugin resource or selects no
   workload branch at all, and the operator only discovers the mistake
   when pods fail to schedule (NVIDIA selector) or when no workload is
   created (silent template branch). The standard mitigation is a JSON
   Schema co-located with `values.yaml`, which `helm install` /
   `helm upgrade` / `helm lint` consult automatically.

2. **`dev/Containerfile` drifted from the post-ADR-0700 layout.**
   ADR-0700 renamed `libvmaf/` → `core/` (and `python/vmaf/` →
   `compat/python-vmaf/` with a shim). The dev-MCP image's stage 3 was
   still doing `COPY --chown=vmaf:vmaf libvmaf/ /build/vmaf/libvmaf/`
   and `cd libvmaf && meson setup build`. Against current master that
   `COPY` fails with `"libvmaf": not found`, breaking the entire
   container build. The drift went unnoticed because the image hadn't
   been rebuilt since the rename merged. CLAUDE.md §15 says rebuild
   "when its image predates the last master sync that touched anything
   under `core/`, `mcp-server/`, `ai/`, `tools/vmaf-tune/`, or `dev/`",
   but until now nothing forced the audit on a schedule.

The audit also surveyed `.dockerignore` (still excluded
`libvmaf/build*/` only; missing the `core/build*/` parallel) and
hadolint findings on the Containerfile (8 pre-existing
DL3003/DL4006/DL3002/DL3009 warnings, none HIGH severity).

## Decision

Land both fixes in one PR because they share a single audit cycle and
the deliverables (changelog fragments, docs/state.md row, ADR-0108
checklist) collapse into one set:

1. **Add `deploy/helm/vmafx/values.schema.json`** covering every
   top-level key in `values.yaml`. Enforce `enum` constraints on the
   three load-bearing fields (`workload`, `gpu.vendor`, `storage.mode`)
   plus `image.pullPolicy`, `service.type`, `persistence.accessMode`,
   `operator.logLevel`, `statefulSet.podManagementPolicy`, and
   `monitoring.serviceMonitor.scheme`. Use `additionalProperties:
   false` on the top level and on each typed sub-object to catch
   sibling-key typos (e.g., `replicaCounts` instead of `replicaCount`).
   Leave `affinity`, `tolerations`, `nodeSelector`,
   `podSecurityContext`, `securityContext`, `livenessProbe`,
   `readinessProbe`, `env`, `envFrom`, `podAnnotations`, `config`,
   and `topologySpreadConstraints` as generic `object` / `array`
   types — they are pass-through structures that the chart hands to
   Kubernetes verbatim, and constraining their shape would conflict
   with upstream `kubectl` semantics every time the API server adds a
   field.

2. **Fix the `dev/Containerfile` ADR-0700 path drift**:
   - `COPY libvmaf/ … libvmaf/` → `COPY core/ … core/`, plus add
     `COPY compat/ … compat/` because the editable Python install
     pulls from `compat/python-vmaf/` via the `python/` shim.
   - `cd libvmaf && meson setup build` → `cd core && meson setup build`
     (two occurrences: the configure step and the `ninja install` step).
   - Update the two comment blocks that still call out
     `libvmaf/src/meson.build` and "meson.build lives in libvmaf/".
   - Extend `.dockerignore` with `core/build*/` siblings to the
     pre-existing `libvmaf/build*/` entries, with a comment naming
     ADR-0700.

The hadolint warnings remain unchanged. They are pre-existing
non-HIGH-severity advisories (DL3003 `cd` in RUN is the standard
multi-stage build idiom for ROCm/NEO/SVT-AV1 source builds where
WORKDIR would break the `&&`-chained cleanup; DL3002 last-USER-root
is forced by the FFmpeg `make install` step that needs root for
`/usr/local/`; DL3009 / DL4006 occur on `apt-mark`-style verification
RUNs that don't fetch lists). Suppressing them line-by-line is a
separate cleanup pass tracked in BACKLOG; the audit's scope is the
ADR-0700 drift, not a hadolint zero-warnings push.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Schema only, defer Containerfile to a separate PR | Smaller diff, narrower review | Two ADR-0108 deliverable rounds for related work; the Containerfile drift is a hard build break, not a nice-to-have | Both fixes share the audit cycle; bundling halves the process overhead |
| Containerfile only, schema deferred | Tighter scope | Schema is the higher-leverage fix (operator-facing typo-catching), and it costs ~5 minutes once the values.yaml has been read | Same logic in reverse: bundling is cheaper |
| Use `helm schema-gen` plugin to auto-generate the schema | No hand-curation effort | The plugin emits the loosest possible schema (every field optional, no enums); misses the typo-catching value entirely | Manual schema is ~250 lines and explicit, which is the point |
| Generate schema from `values.yaml` comments via a build step | Future-proof against values.yaml changes | New tooling dependency for one chart; the chart's top-level surface is stable enough that a hand-written schema is fine for now | Defer until a second chart appears |
| Replace `COPY libvmaf/` with `COPY . .` (whole repo) | Simplest fix | Re-introduces the `.corpus/` (781 GB) leak the explicit COPY list was added to prevent (see comment block above the COPY lines) | Hard regression of an earlier fix |

## Consequences

- **Positive**: `helm install vmafx … --set gpu.vendor=qualcomm`
  fails fast with a clear error message
  (`at '/gpu/vendor': value must be one of 'nvidia', 'amd', 'intel',
  'cpu'`) before any manifest is rendered. The dev-MCP container
  builds clean against current master without manual path rewrites.
- **Positive**: `additionalProperties: false` on typed sub-objects
  catches `replicaCounts` / `repostiory` / `maxSurg` typos at
  install time.
- **Negative**: every future addition to `values.yaml` now requires a
  matching schema entry, or `helm lint` will fail. The schema needs
  to be kept in lockstep with the chart; a CI check could enforce
  this in a follow-up.
- **Neutral / follow-ups**:
  - Add a CI gate that diffs `values.yaml` keys against
    `values.schema.json` properties on PRs that touch the helm chart.
  - Address hadolint advisories (DL3003 / DL4006 / DL3002 / DL3009)
    in a dedicated cleanup pass; they are non-HIGH-severity and out
    of scope here.

## References

- See [ADR-0700](0700-vmafx-repo-layout.md) for the `libvmaf/` →
  `core/` rename that the Containerfile drift came from.
- See [ADR-0703](0703-vmafx-server-go-grpc.md) for the chart's
  `image.repository` target (`ghcr.io/vmafx/vmafx-server`).
- See [ADR-0714](0714-vmafx-operator-skeleton.md) for the operator
  deployment that the schema's `operator` block validates.
- See [ADR-0719](0719-vmafx-node-rclone-integration.md) for the
  `storage.mode` enum and the rclone config secret surface.
- See [ADR-0726](0726-drop-vulkan-backend.md) — `gpu.vendor` enum
  omits `vulkan` because the backend was removed.
- See [ADR-0738](0738-bump-cuda-133-r610-local.md) for the
  R610.43.02 driver floor referenced in `values.yaml` GPU comments.
- See `CLAUDE.md §15` for the dev-mcp container rebuild policy this
  audit enforces.
- Source: req (audit dispatch 2026-05-30, "Audit Helm chart values
  schema enforcement + verify the vmaf-dev-mcp container image still
  builds clean against current master").
