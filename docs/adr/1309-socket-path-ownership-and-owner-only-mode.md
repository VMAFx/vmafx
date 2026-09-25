<!-- markdownlint-disable MD013 MD060 -->

# ADR-1309: Owner-only sidecar socket with identity-checked lifecycle

- **Status**: Accepted
- **Supersedes**: Partially supersedes [ADR-1222](1222-code-scanning-alert-triage-and-scope.md) (only for Alert 946)
- **Date**: 2026-09-24
- **Deciders**: Lusoris
- **Tags**: `ai`, `security`, `sidecar`, `unix-socket`, `concurrency`

## Context

GitHub Code Scanning Alert 946 reported that the online-training sidecar created
its unauthenticated Unix-domain socket with group-write permission (`0o660`).
[ADR-1222](1222-code-scanning-alert-triage-and-scope.md) retained that mode on
the premise that production ran the Python sidecar and Go node under different
UIDs but a shared GID.

Current repository evidence does not support that premise. The Helm helper in
`deploy/helm/vmafx/templates/sidecar-trainer.yaml` names a removed
`node-deployment.yaml` consumer, `templates/node.yaml` does not include the
helper, `values.schema.json` rejects the undocumented `sidecar.*` values, and
the pod security context uses one UID/GID. The different-UID/same-GID test proved
only that Unix group permissions work; it did not establish a deployed product
requirement.

The old lifecycle also blindly unlinked the configured pathname before bind and
again during cleanup. A second server could detach a live listener, and an
exiting server could remove a replacement object it did not create. A
connect-only stale check is insufficient during the bind-before-listen window,
where a live socket may return `ECONNREFUSED`.

## Decision

1. Create the socket with mode exactly `0o600`. The shipped endpoint is
   unauthenticated and the current product contract requires only same-UID
   access. Any future group-shared mode needs an explicit configuration surface,
   working chart wiring, an authenticated-or-equivalent threat model, and
   end-to-end tests.
2. Hold a non-blocking exclusive `flock` on an adjacent owner-only `.lock` file
   for the complete server lifetime. Open it with `O_NOFOLLOW` where available
   and verify through `fstat`/`lstat` that the descriptor and pathname identify
   the same regular file.
3. Inspect the socket pathname with `lstat`. Refuse symlinks and all non-socket
   objects without modifying them. Probe a socket in non-blocking mode so a
   live listener with a full accept queue cannot block startup indefinitely.
4. Treat only `ECONNREFUSED` as a stale candidate. Treat `EAGAIN`,
   `EINPROGRESS`, timeouts, and every other pending/unverified result as active
   (`EADDRINUSE`). Re-read a stale candidate and unlink it only if it is still a
   socket with the same device/inode identity.
5. Record the bound socket's device/inode identity, apply `0o600` without
   following symlinks, verify identity before listen, and unlink during cleanup
   only if the current pathname is still that owned socket.

The lifetime claim closes races among cooperating sidecar servers, including
bind-before-listen. POSIX has no portable atomic compare-and-unlink operation;
an uncooperative process with write permission on the parent directory can
still replace the pathname between the last identity check and `unlink`.
Parent-directory permissions therefore remain part of the security boundary.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Owner-only `0o600` plus identity-checked lifecycle (chosen)** | Least privilege for the current topology; removes Alert 946 at source; preserves active and replacement objects | Future cross-UID peers need an explicit feature; leaves the unavoidable portable unlink race against an uncooperative directory writer | Matches the only deployed access requirement and fails closed wherever portable POSIX APIs permit. |
| Keep unconditional `0o660` | Synthetic same-GID peers can connect | Grants write access without a deployed requirement; retains Alert 946; protocol has no separate authentication | The documented production topology does not exist in the current chart. |
| Add a group-sharing option now | Could support a future cross-UID topology explicitly | Adds unused product/API/chart surface and a security policy without an actual consumer | Defer until the topology and threat model are concrete. |
| Reject every pre-existing socket, including stale ones | Simplest pathname policy | Requires manual recovery after crashes and stale filesystem entries | Safe stale recovery is possible with the claim and identity checks. |

## Consequences

- **Positive**:
  - Alert 946 is eliminated at source with no Semgrep suppression.
  - A second cooperating server cannot detach a live or bound-but-not-listening
    endpoint.
  - A full live listener backlog is refused within a bounded probe instead of
    blocking server startup.
  - Symlinks, ordinary files, and replacement sockets/files are preserved.
- **Negative**:
  - Different-UID peers cannot connect until an explicit group-sharing feature
    is designed and wired.
  - The adjacent `.lock` file persists as an owner-only coordination object.
- **Neutral / follow-ups**:
  - Hosted alert closure still depends on the post-merge Code Scanning run.
  - Parent-directory ownership remains part of the Unix pathname security model.

## References

- [ADR-1222: Code-scanning alert triage and scope](1222-code-scanning-alert-triage-and-scope.md)
- [ADR-1307: Pure SHA-256 memoization keys with cold cache invalidation](1307-sha256-memoization-cache-invalidation.md)
- [Research-2095: Semgrep Warning Alerts 946–949 Audit and Resolution](../research/2095-semgrep-warning-alerts-946-949-audit.md)
- [ADR-0535: Atomic ADR allocator](0535-adr-atomic-allocator.md)
- Source: `req` — "well we have a lot of warnings lol as well..."
