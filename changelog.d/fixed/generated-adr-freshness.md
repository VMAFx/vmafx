- Refresh ADR navigation and all tag indexes from their sources, remove duplicate
  navigation entries, and restore the missing ADR-1123 index row.
- Check generated metadata freshness and fragment coverage locally and in required
  Docs CI; reject malformed sentinel pairs before navigation writes.
- Preserve readable generated titles and tag names, and skip Pages deployment
  when the impact plan produced no documentation artifact.
