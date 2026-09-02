<!-- markdownlint-disable MD013 MD041 -->
# Research-1052: ARM motion_v2 re-registration bisect

No digest needed: trivial. The introducing commit (6bb5464511 / PR #532) is
identified by bisect; the root cause is a missing source-file registration
and a test that assumed pre-port extract()-emits-motion2 semantics. No
alternative approaches were evaluated.
