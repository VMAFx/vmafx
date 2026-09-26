<!-- markdownlint-disable MD013 MD060 -->
# Research-2104: GPU option-alias parity — 2026-09-25

**Status:** Complete

**Scope:** option-table identity for equivalent CPU, CUDA, SYCL, HIP, and
Metal feature extractors. No kernel, model, snapshot, dependency, benchmark,
training, or Netflix golden-data change.

## Why an alias is behavior

`feature_name.cpp` appends a non-default feature parameter by its `alias`.
ADR-1183 then uses those feature identities when a model selects a GPU twin.
An absent or different twin alias therefore changes the published dictionary
key even when the option name, value, and computation are otherwise equal.

## Reconciled inventory

The ledger's eighteen-site inventory was compared against the exact collector
parent. Eight sites were already aligned: all six CUDA/SYCL/HIP `float_adm`
`scf` / `scfd` aliases, SYCL `float_motion` `force_0`, and SYCL
`integer_vif` `ssclz`. The first run of the new contract failed ten subtests:

| Family | Missing aliases on the parent | Count |
| --- | --- | ---: |
| `float_motion.motion_force_zero` | CUDA, HIP | 2 |
| `integer_motion.motion_force_zero` | CUDA, SYCL, HIP | 3 |
| `float_vif.vif_kernelscale` | CUDA, SYCL, HIP | 3 |
| `integer_vif.vif_skip_scale0` | CUDA, HIP | 2 |
| **Total** | | **10** |

Adding only the CPU-authoritative aliases makes all eighteen subtests pass.

## Consumer audit

A live-tree search across `model/`, `python/`, `test/`, `testdata/`, and user
documentation found canonical `_force_0`, `_ks_`, and `_ssclz` consumers.
No live consumer depended on a missing alias or on the retired ADM `cs` /
`cds` spellings. Models continue to specify canonical option names; no model
or score snapshot changes are required.

## Regression shape

`core/test/test_gpu_option_alias_contract.py` reads each production source,
finds exactly one named option, extracts its balanced initializer, and checks
its alias. Registering it in Meson's fast suite gives every host a check over
the whole known inventory without pretending to replace hardware parity.

## Separate capability finding

Alias identity is not device capability. All four GPU `float_vif` twins
advertise `vif_kernelscale` but reject every non-default value in `init()`.
All four GPU `float_adm` twins likewise advertise `adm_csf_mode` but reject
every mode except `0`. Metal `integer_adm` is a third affected family: it
advertises the CPU's `adm_csf_mode` range but rejects every nonzero mode even
though the CUDA, SYCL, and HIP integer twins implement the supported CPU
configurations. ADR-1183 consequently selects those twins and receives
`-EINVAL` instead of falling back to CPU implementations that support the
requested values (subject to their ordinary representability guards). CUDA
`float_vif` also lacks `vif_skip_scale0`, which does trigger the safe CPU
fallback. A literal-value restriction scan across the four GPU feature trees
found those three families; the value-capability failure and a required
semantic audit of every similar init-time restriction are recorded separately
in `docs/state.md`. Closing the alias-only item does not hide them.

## Result

The audited aliases are now one explicit compatibility set. The change does
not alter arithmetic or the default path, and the source contract fails if
any one of the eighteen spellings drifts again.
