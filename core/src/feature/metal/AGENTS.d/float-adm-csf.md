---
paths:
  - core/src/feature/metal/float_adm_metal.mm
invariant: float_adm_metal takes its CSF weights from CPU (ADR-1489, ADR-1498).
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# `float_adm_metal.mm`: CSF weights from the CPU (ADR-1489, ADR-1498)

- No copy of `dwt_quant_step()` since ADR-1498: weights =
  `adm_csf_rfactor_s()` (`adm_float_reference.h`) with every CPU option,
  `adm_f1sN` / `adm_f2sN` included. Never bring a local step back. Guard:
  `core/test/test_float_adm_csf_upstream_contract.py`.
