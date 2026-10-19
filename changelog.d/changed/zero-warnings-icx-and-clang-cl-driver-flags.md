- **icx and the clang-cl style drivers stop warning about our own compile flags.** `icx` and `icpx`
  reported `-ffp-contract=off` after `-fp-model=precise` as `-Woverriding-option` on every compile
  (7,300 times in one CI leg). The strict policy now spells `-fp-model=precise -fno-fast-math
  -fcomplex-arithmetic=full -ffp-contract=off`; on 182 translation units of this tree the objects
  are byte-identical to the old spelling ([ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md)).
  clang-cl and icx-cl are no longer offered `-pedantic`, `-fvisibility=hidden` and
  `-fvisibility-inlines-hidden`, which they ignored with a warning per compile.
