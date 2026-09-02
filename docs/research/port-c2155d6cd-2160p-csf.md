# Research Digest: 2160p at 1.5H CSF support (upstream c2155d6cd)

**Port date:** 2026-06-01
**Upstream author:** Christos Bampis, Netflix
**Upstream SHA:** c2155d6cdc1796329783dcb9ce8ce5cf3d7e1dc5

## Summary

The 2160p display at 1.5x picture height viewing distance has the same
angular resolution as 1080p at 3H: `1.5 * 2160 = 3.0 * 1080 = 56.55 ppd`.
`barten_watson_blend_csf{,_mae}` previously fell through to the else-branch
(assertion or undefined behaviour) for this combination. The fix adds the
equivalence branch before the existing `2160@3H` branch.

## Validation

Upstream adds 16 pinning assertions in `test_barten_csf.c` verifying
byte-for-byte equality between `csf(scale, theta, 1.5, 2160)` and
`csf(scale, theta, 3.0, 1080)` for all 8 (scale, theta) combinations.
These tests are ported verbatim.
