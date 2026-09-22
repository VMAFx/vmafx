- **README now carries the managed Praetor governance block that
  `praetorctl audit` requires.** The pinned engine bump to `7c0f803d4` pulled
  in praetor #411 (`fix(readme)!: enforce truthful managed governance
  blocks`), which turned the README governance surface into a
  marker-delimited region (`<!-- praetor:readme-governance:start -->` …
  `:end -->`) that the audit verifies byte-for-byte against a deterministic
  rendering; until it existed, `praetorctl audit` failed with "managed README
  governance block is missing". The block deliberately states what each gate
  enforces instead of certifying a passing build: it records the adoption
  state, points at the `.standards-baseline.json` debt anchor (989 recorded
  infractions, growth forbidden), and says in its own first line that it is
  not a verification certificate. Every hand-written README surface is
  untouched — the HISS-21 badge, the prose "Standards & Governance" section
  and the badge wall all survive, because the engine detects the
  repository's own HISS badge and omits the block's generated one rather
  than duplicating it.
