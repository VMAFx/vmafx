- **Source ADR citations now retain exact decision provenance.** The completed
  audit corrects five additional proven number drifts: ADR-0049 to ADR-0415
  (CAMBI SYCL), ADR-0322 to ADR-0326 (`vmaf-tune` Phase B), ADR-0572 to
  ADR-0574 (CUDA float-ADM AIM slots), ADR-0715 to ADR-0714 (operator
  skeleton), and ADR-0814 to ADR-0786 (operator Stage-2 reconcilers).
  ADR-1214 now exists and needs no rewrite. False “CPU-only” SpEED comments
  now name the shipped ADR-0567/0964/0965 GPU twins.

  `scripts/ci/check-source-adr-citations.py` and its committed registry bind
  every selected implementation/build citation to the exact ADR filename and
  exact source-site counts. The always-run gate rejects missing numbers,
  filename reallocation, source-site drift, reused retired numbers, and escaped
  synthetic fixtures. ADR-0557/0558, ADR-0722, and ADR-0864 remain reserved,
  evidence-backed historical identities instead of being falsely repointed.
  Disposable fixture Git now strips inherited repository variables and caller
  configuration, preventing a pre-commit alternate index from being replaced
  by fixture paths.
