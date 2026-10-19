- Build: the Windows MinGW UCRT64 build compiles again. The VMAFx API's printf-format
  attribute now names MinGW's own archetype (`__MINGW_PRINTF_FORMAT`), so GCC accepts `%zu`
  under UCRT instead of failing with `-Werror=format`.
