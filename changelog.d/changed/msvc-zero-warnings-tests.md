- The unit tests compile without an MSVC warning (about 71,000 per Windows job
  before): float tables carry the `f` suffix (every literal checked to equal the
  value the implicit double-to-float conversion gave), narrowing conversions are
  explicit, and C test cases are declared `(void)`. No test value or tolerance
  changed.
