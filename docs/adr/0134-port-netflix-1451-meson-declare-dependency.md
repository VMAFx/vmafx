<!-- markdownlint-disable MD013 MD060 -->
# ADR-0134: Port Netflix#1451 — `meson declare_dependency` + `override_dependency` for libvmaf

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: @lusoris, Claude
- **Tags**: build, upstream-port

## Context

Upstream PR [Netflix#1451][pr1451] (open, 1 commit, +6/-0) adds the
meson idioms that let `libvmaf` be consumed as a meson subproject
without downstream glue.

Without this, a consuming project that does

```meson
libvmaf_proj = subproject('libvmaf')
libvmaf_dep  = libvmaf_proj.get_variable('libvmaf_dep')  # fails
```

has no exported dependency to pick up. The workaround — manually
reaching into the subproject with `declare_dependency(link_with: ...)`
from the parent — leaks the subproject's include layout into the
parent's build file and breaks whenever upstream reshuffles headers.

The idiomatic fix is two lines at the end of `core/src/meson.build`:

```meson
libvmaf_dep = declare_dependency(
    link_with: libvmaf,
    include_directories: [libvmaf_inc],
)
meson.override_dependency('libvmaf', libvmaf_dep)
```

`declare_dependency` names the exported thing; `override_dependency`
registers it under the pkg-config name so any `dependency('libvmaf',
...)` in a parent project resolves to this local build of libvmaf
when available as a subproject — the standard meson pattern.

## Decision

Port upstream's diff substance into
[`core/src/meson.build`](../../core/src/meson.build) just after
the existing `pkg_mod.generate(...)` block. One deviation: the fork
terminates the `include_directories:` argument with a trailing comma
(meson style convention throughout the fork's build files) whereas
upstream's patch does not. Behaviourally identical.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port upstream diff verbatim | Smallest diff from upstream | Trailing-comma-omitted style clashes with every other multi-arg call in the fork's build files; `/sync-upstream` will rewrite it anyway | Chosen fork-style with trailing comma to avoid stylistic churn |
| Skip the `meson.override_dependency` call and only declare | Shorter patch | Defeats the PR's goal — `dependency('libvmaf')` in a parent project would still fall through to a system lookup instead of the local subproject build | Partial port loses the user-visible benefit |
| Wait for upstream merge | No fork-local diff | Upstream PR has been open since 2025-07-10 with no merge signal, and the fork already has a "subproject-friendly" aspiration in the build-system docs | Unbounded wait on a 6-line port |

## Consequences

- **Positive**: Consumers can now use the fork as a meson subproject
  with the standard idiom (`dependency('libvmaf')`). The override
  registration means parent-project `dependency()` calls find the
  local subproject build instead of falling through to a system
  libvmaf. pkg-config generation is unchanged.
- **Negative**: None material. The two new symbols are bottom-of-file
  and don't affect the existing `libvmaf` target or `pkg_mod.generate`
  output.
- **Neutral / follow-ups**:
  - When Netflix merges #1451, `/sync-upstream` will see a near-clean
    merge (only the trailing-comma stylistic drift to reconcile);
    note in [`rebase-notes.md`](../rebase-notes.md).
  - No doc change required under `docs/development/` —
    build-as-subproject is not a user-discoverable *fork-added*
    surface (ADR-0100 §Per-surface bars only triggers on fork-added
    surfaces; this is a port of a Netflix-proposed convention).

## References

- Upstream PR: [Netflix#1451 — build: declare dependency to use it as subproject][pr1451]
- Backlog: [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md) T4-5
- Meson docs: [`declare_dependency`](https://mesonbuild.com/Reference-manual_functions.html#declare_dependency),
  [`meson.override_dependency`](https://mesonbuild.com/Reference-manual_builtin_meson.html#mesonoverride_dependency)
- Source: `req` — user direction to ship Batch-A (T0-1 + T4-4/5/6) as
  one PR (2026-04-20 popup).

[pr1451]: https://github.com/Netflix/vmaf/pull/1451
