- Perceptual weighting: Pelorus side data that libx265 wrote behind its own
  SEI UUID is read again instead of being ignored as foreign. Only the x265
  UUID is accepted as an outer wrapper; any other outer UUID still leaves the
  frame unweighted ([perceptual weighting API](docs/api/perceptual-weight.md),
  ADR-3340).
