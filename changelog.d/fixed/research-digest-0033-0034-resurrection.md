- Remove stale 0033/0034 research-digest twins that a collector merge
  resurrected after their historical rename to 0432/0433, restore every
  authoritative link, and add ADR-1335's trusted-merge-base ratchet. The gate
  rejects baseline deletion or laundering, new numeric-ID collisions, and
  filename/H1 drift while allowing reviewed legacy-debt reductions; the first
  trusted comparison also repairs the train-era 2080 collision and malformed
  Research-1306/1317 H1s.
