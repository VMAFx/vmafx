## ADR claim lock independent of the working directory (2026-10-13)

`scripts/adr/next-free.sh` builds its lock name from the resolved absolute
path of the common git directory, records an owner token in the lock and
decides each number with an exclusive claim file under `.git/adr-claims/`.
A sync or cleanup must not bring back a lock name from the relative output of
`git rev-parse --git-common-dir`, a release that removes a lock it does not
own, or a claim file written without `noclobber`. The tests that clean up the
lock (`scripts/adr/test-next-free.sh`,
`scripts/adr/tests/test-next-free-remote-aware.sh`) compute the same name.
`scripts/adr/tests/test-next-free-concurrent.sh` guards it.
