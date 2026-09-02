- **docs(adm)**: replace open `//TODO: if we integrate adm_p_norm` marker in
  `integer_adm.c` with a formal DEFERRED block citing ADR-0481; documents why
  `adm_p_norm` remains hardcoded at 3.0 and what must happen before it can be
  wired (retrained model + snapshot regeneration).
