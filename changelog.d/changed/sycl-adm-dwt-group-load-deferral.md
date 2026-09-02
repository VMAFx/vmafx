- ADR-0332 records the deferral of the SYCL ADM DWT `group_load`
  rewrite recommended by research-0086 §A.4. The kernel
  ([`integer_adm_sycl.cpp`](core/src/feature/sycl/integer_adm_sycl.cpp))
  is unchanged at runtime; the deferral surfaces a divisibility
  blocker (`TILE_ELEMS / WG_SIZE = 576 / 256 = 2.25`, not integer)
  and a source-contiguity blocker (multi-row tile is non-contiguous)
  that defeat the digest's sketched rewrite shape. ADR-0202 gains a
  Status-update appendix recording the investigation outcome per
  the ADR-0028 immutability rule. No user-visible behavioural delta;
  no score change.
