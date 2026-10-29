<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3385: Map engineering principles to every shipped language

- **Status**: Accepted
- **Date**: 2026-10-28
- **Deciders**: lusoris
- **Tags**: docs, governance, safety, api

## Context

[Engineering principles](../principles.md) has detailed C rules but only a short language-role table. [Issue #2438](https://github.com/VMAFx/vmafx/issues/2438), RC4 work package 14, requires language-specific safety requirements, an applicability matrix and demonstrated enforcement. [ADR-2342](2342-rc-map-amendment-2026-10.md) records Q-060 and Q-061: extend the principles across languages, make warnings fatal and prove missing gates with planted violations. The old language policy also predates the generated RC4 API, and its independent-approval requirement conflicts with the single-maintainer model in [ADR-2126](2126-scorecard-single-maintainer-exceptions.md).

The first slice is documentation only. Existing partial scanner claims in [`.config/hiss/coverage.yaml`](../../.config/hiss/coverage.yaml) cannot be promoted to whole-language enforcement claims. C rules in sections 1–7 must retain their substance; setup already belongs in [the language guide](../development/languages.md).

## Decision

Keep sections 1–7 intact, replace section 8 with roles, standards, verified checks and safety mappings for every tracked language, and add a rule-by-language applicability matrix in section 9. Mark incomplete enforcement explicitly as “no gate yet” for the later gate slice of #2438. Keep installation guidance in the language guide and policy in the principles. Update the API boundary to `libvmafx.so.1`, compatibility `libvmaf.so.3` and bindings generated from `core/api/vmafx.toml`; align revision approval with ADR-2126 in section 10.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Apply the C wording unchanged to every language | One short policy | Imposes C pointers, macros and assertion density on data formats and managed runtimes | Applicability would be misleading |
| Put independent standards and gate tables in each language guide | Local detail | Duplicates the shared policy and existing setup guide; updates can disagree | Conflicts with HISS-19 reuse |
| Add all missing gates in this slice | Enforcement and prose land together | Expands a docs review into toolchain changes and planted-defect test work | The authorized first slice is docs only |
| One policy, applicability matrix and explicit gaps (chosen) | Preserves C requirements and makes follow-up obligations reviewable | The inventory needs maintenance when gates change | Chosen; #2438 remains open for enforcement |

## Consequences

- **Positive**: contributors can distinguish a requirement, a working scoped check and an enforcement gap; the API and revision policy match their governing ADRs.
- **Negative**: a wide matrix and explicit partial coverage require careful review; this slice supplies no new protection.
- **Neutral / follow-ups**: the gate slice must replace each missing check with a real command/config and a planted violation proving rejection. New languages require their own role decision and checks. No runtime, ABI or build behavior changes in this slice.

## References

- [Issue #2438](https://github.com/VMAFx/vmafx/issues/2438): RC4 WP14 scope and gate-proof acceptance criteria.
- Q-060 and Q-061, 2026-10-06, as recorded by [ADR-2342, WP14](2342-rc-map-amendment-2026-10.md): apply engineering principles by language, require fatal diagnostics and demonstrate missing checks with planted defects (paraphrased).
- [ADR-1852](1852-vmafx-api-redesign.md), [API guide](../api/vmafx/index.md) and [generation guide](../development/api-generation.md): the shared generated interface.
- [ADR-2126](2126-scorecard-single-maintainer-exceptions.md): single-maintainer review exceptions and revisit conditions.
- [ADR-0702](0702-vmafx-phase4-language-modernization.md): language roles; [ADR-1142](1142-whole-codebase-standards.md): whole-tree scope.
