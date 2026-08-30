<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1101: Change vmaf container user GID/UID from 1000 to 2000

- **Status**: Accepted
- **Date**: 2026-06-08
- **Deciders**: Lusoris
- **Tags**: `build`, `ci`, `workspace`

## Context

Ubuntu 26.04 (Resolute Raccoon) ships a built-in `ubuntu` user and group
at GID/UID 1000 in the base image. The `dev/Containerfile` Stage 1 ran
`groupadd --gid 1000 vmaf`, which exits 4 ("GID in use") on any Ubuntu 26.04
base layer because 1000 is already occupied by the `ubuntu` group. This
silently blocked every container rebuild after the base image was updated
to Ubuntu 26.04 in ADR-0603.

The BuildKit cache-mount directives for the ccache layer also referenced
`uid=1000,gid=1000`, which would map to the wrong identity after any
workaround that picks a different UID/GID at `useradd` time.

## Decision

We will change the `vmaf` user and group to GID/UID 2000. GID/UID 2000 is
in the local/static allocation range on all Ubuntu LTS releases (Ubuntu
reserves only up to 999 for system accounts and 1000 for the first
interactive user). All four references — `groupadd --gid`, `useradd --uid`,
and the two `--mount=type=cache,uid=…,gid=…` BuildKit directives — are
updated atomically to 2000.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Delete the `ubuntu` built-in user/group at image start | Removes the conflict at source | Fragile — Ubuntu may re-create it; unsupported mutation of the base layer | Rejected |
| Use `--no-user-group` + let `useradd` pick the next free UID/GID dynamically | No hard-coded ID | Non-deterministic; BuildKit cache mounts require a fixed numeric ID | Rejected |
| Stay on Ubuntu 24.04 base | No conflict at GID 1000 | Misses Ubuntu 26.04 glibc 2.43 + Python 3.14 required by ADR-0603 | Rejected |

## Consequences

- **Positive**: Container builds succeed on the Ubuntu 26.04 base without
  manual workarounds. BuildKit ccache mounts resolve to the correct user
  identity. No functional change to the running container.
- **Negative**: Any host-side bind-mount or volume that was chown'd 1000:1000
  for the vmaf user will need to be re-chown'd to 2000:2000. In practice
  only the `/probes/` bind-mount in the dev compose file is affected; the
  compose file itself manages the directory creation via the entrypoint
  script which runs as root before dropping to the vmaf user.
- **Neutral / follow-ups**: The `dev/docker-compose.yml` does not hard-code
  UID/GID; the entrypoint script creates `/probes/vmaftune-work` as root
  before switching to the vmaf user — no changes required there.

## References

- ADR-0603: Ubuntu 26.04 base image adoption.
- ADR-0790: Containerfile layer optimisation (context for Stage 3 cleanup).
- Ubuntu manpage `addgroup(8)`: GIDs below 1000 are system accounts; 1000
  is the first interactive user slot; values ≥ 1000 are user-allocated.
- Memory entry: `dev/Containerfile still has post-rename stale libvmaf/ refs`
  (2026-06-01 session — also audited during this fix; no stale refs remain).
