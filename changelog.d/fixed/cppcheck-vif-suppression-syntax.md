- `core/src/feature/vif.c`: corrected 10 `cppcheck-suppress
  invalidPointerCast` comments from bracket syntax
  `[MISRA-C:2012-11.3/EXP36-C: ...]` to semicolon-delimited syntax
  `; MISRA-C:2012-11.3/EXP36-C: ...`. Bracket syntax is not
  recognised by cppcheck and caused `preprocessorErrorDirective`
  warnings; semicolon is the correct inline-comment delimiter per
  the cppcheck documentation.
