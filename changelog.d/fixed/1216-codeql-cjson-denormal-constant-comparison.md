- Resolved CodeQL warning alert 1216 (`cpp/constant-comparison`) in `core/test/test_cjson.c`:
  the test for printing `DBL_TRUE_MIN` now semantically inspects the formatted
  string produced by `cJSON_CreateNumber()` and asserts that it matches either
  supported representation (`"0"` under DAZ arithmetic such as `icx -fp-model=fast` or
  `"4.94065645841247e-324"` under standard IEEE-754 subnormals), removing the dead
  floating-point zero comparison while preserving complete precision coverage.
