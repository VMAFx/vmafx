- **Feature options with fractions work in any numeric locale.** A program
  that set a decimal-comma locale (`setlocale(LC_ALL, "")` under `de_DE`,
  `fr_FR` and similar) could not use the default model `vmaf_v1.0.16_3d0h`:
  `vmaf_use_features_from_model()` returned `-EINVAL`, because the option
  parser read `0.7` with `strtod()` in the caller's locale and stopped at the
  period. The feature dictionary's number normalisation had the same fault and
  could store `0.02` as `0` or `0,02`. Both now parse and format option numbers
  in the C locale on the calling thread only, as the model reader and the report
  writers already do; the caller's locale is left as it was.
